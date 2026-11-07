#pragma once

#include <curl/curl.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "concurrent_queue_include_helper.h"

struct HttpResponse {
    int status_code = 0;
    std::string body;
    CURLcode curl_code = CURLE_OK;
};

struct CompletionSlot {
    HttpResponse response;
    std::atomic<bool> ready{false};

    void signal(HttpResponse resp) {
        response = std::move(resp);
        ready.store(true, std::memory_order_release);
        ready.notify_one();
    }

    HttpResponse& wait() {
        // Park in the kernel on a futex via std::atomic<bool>::wait
        // until signal() publishes the response. libstdc++ does a brief
        // internal spin before sleeping, so fast completions stay fast;
        // slow network waits do NOT burn a core.
        while (!ready.load(std::memory_order_acquire)) {
            ready.wait(false, std::memory_order_acquire);
        }
        return response;
    }
};

struct HttpRequest {
    std::string url;
    std::string method;  // "GET", "POST", "DELETE", "PUT"
    std::string body;
    std::vector<std::string> headers;  // "Key: Value" format
    long timeout_ms = 30000;
    long connect_timeout_ms = 5000;
};

class CurlMultiManager {
   public:
    static CurlMultiManager& instance();

    // Submit an async HTTP request, returns a future for the response
    std::future<HttpResponse> submit(HttpRequest req);

    // Synchronous wrapper: submits and blocks until complete.
    // Returns {status_code, body} matching existing call sites.
    std::pair<int, std::string> execute(HttpRequest req);

    // Pre-warm a connection to the given URL (issues a lightweight GET)
    void warmup(const std::string& url);

    // Pin a hostname to a specific IP, bypassing DNS resolution on the hot path.
    // Format: "host:port:ip" — e.g., "clob.polymarket.com:443:104.18.32.45"
    // Thread-safe: can be called before or after IO thread starts.
    void add_dns_pinning(const std::string& host, int port,
                         const std::string& ip);

    // Start a periodic heartbeat to keep connections alive at the application
    // layer. Sends a lightweight GET every interval_sec seconds.
    void start_heartbeat(const std::string& url, int interval_sec);

    // Shut down the I/O thread and clean up resources
    void shutdown();

    ~CurlMultiManager();

    CurlMultiManager(const CurlMultiManager&) = delete;
    CurlMultiManager& operator=(const CurlMultiManager&) = delete;

   private:
    CurlMultiManager();

    struct InFlightRequest {
        CURL* easy = nullptr;
        struct curl_slist* header_list = nullptr;
        std::string response_body;
        // One of these is set depending on submission path
        std::promise<HttpResponse> promise;
        CompletionSlot* slot = nullptr;  // Non-owning; null for async submit()
    };

    struct PendingEntry {
        HttpRequest request;
        std::promise<HttpResponse> promise;
        CompletionSlot* slot = nullptr;  // Non-owning; null for async submit()
    };

    // Wake the IO thread only if it's actually sleeping
    void wake_io_thread();

    // Event loop running in io_thread_
    void event_loop();

    // Drain pending_queue_ and add requests to curl_multi
    void process_pending_requests();

    // Set up a curl easy handle from a request
    void setup_easy_handle(PendingEntry& entry);

    // Check for completed transfers and fulfill promises/slots
    void check_multi_info();

    // Fire heartbeat requests
    void fire_heartbeat();

    // Handle pool — IO thread only, no synchronization needed
    CURL* acquire_handle();
    void release_handle(CURL* h);

    // curl_multi callback: socket state changes → epoll
    static int socket_callback(CURL* easy, curl_socket_t s, int what,
                                void* userp, void* socketp);

    // curl_multi callback: timeout changes → timerfd
    static int timer_callback(CURLM* multi, long timeout_ms, void* userp);

    // Write callback for curl easy handles
    static size_t write_callback(char* ptr, size_t size, size_t nmemb,
                                  void* userdata);

    CURLM* multi_ = nullptr;
    int epoll_fd_ = -1;
    int wakeup_fd_ = -1;      // eventfd
    int timer_fd_ = -1;       // timerfd (curl timeouts)
    int heartbeat_fd_ = -1;   // timerfd (periodic heartbeat)

    std::thread io_thread_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> worker_awake_{false};

    // Lock-free MPSC queue (multiple callers, single IO thread consumer)
    moodycamel::ConcurrentQueue<PendingEntry> pending_queue_;

    // Track in-flight requests by easy handle pointer
    std::unordered_map<CURL*, std::unique_ptr<InFlightRequest>> in_flight_;

    // Easy handle pool — IO thread only
    std::vector<CURL*> handle_pool_;
    static constexpr size_t MAX_POOL_SIZE = 32;

    // DNS pinning resolve list — set once at startup, attached to every handle
    struct curl_slist* resolve_list_ = nullptr;

    // Heartbeat URL (empty = disabled)
    std::string heartbeat_url_;
};
