#include "curl_multi_manager.h"

#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <iostream>

static constexpr int MAX_EPOLL_EVENTS = 64;

// Sentinel pointer values to distinguish eventfd/timerfd from curl sockets
// in epoll events. These are never dereferenced.
static void* const WAKEUP_SENTINEL = reinterpret_cast<void*>(1);
static void* const TIMER_SENTINEL = reinterpret_cast<void*>(2);
static void* const HEARTBEAT_SENTINEL = reinterpret_cast<void*>(3);

CurlMultiManager& CurlMultiManager::instance() {
    static CurlMultiManager inst;
    return inst;
}

CurlMultiManager::CurlMultiManager() {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("CurlMultiManager: epoll_create1 failed");
    }

    wakeup_fd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeup_fd_ < 0) {
        close(epoll_fd_);
        throw std::runtime_error("CurlMultiManager: eventfd failed");
    }

    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd_ < 0) {
        close(wakeup_fd_);
        close(epoll_fd_);
        throw std::runtime_error("CurlMultiManager: timerfd_create failed");
    }

    heartbeat_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (heartbeat_fd_ < 0) {
        close(timer_fd_);
        close(wakeup_fd_);
        close(epoll_fd_);
        throw std::runtime_error("CurlMultiManager: heartbeat timerfd failed");
    }

    // Add eventfd to epoll
    struct epoll_event ev {};
    ev.events = EPOLLIN;
    ev.data.ptr = WAKEUP_SENTINEL;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);

    // Add timerfd to epoll
    ev.data.ptr = TIMER_SENTINEL;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);

    // Add heartbeat timerfd to epoll (initially disarmed)
    ev.data.ptr = HEARTBEAT_SENTINEL;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, heartbeat_fd_, &ev);

    // Initialize curl_multi
    multi_ = curl_multi_init();
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, timer_callback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, 128L);

    io_thread_ = std::thread(&CurlMultiManager::event_loop, this);
}

CurlMultiManager::~CurlMultiManager() { shutdown(); }

void CurlMultiManager::shutdown() {
    if (shutting_down_.exchange(true)) return;  // Already shutting down

    // Wake up the event loop unconditionally for shutdown
    uint64_t val = 1;
    if (::write(wakeup_fd_, &val, sizeof(val)) < 0) {
    }

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    // Fail any pending requests
    {
        PendingEntry entry;
        while (pending_queue_.try_dequeue(entry)) {
            HttpResponse resp;
            resp.curl_code = CURLE_ABORTED_BY_CALLBACK;
            if (entry.slot) {
                entry.slot->signal(std::move(resp));
            } else {
                entry.promise.set_value(std::move(resp));
            }
        }
    }

    // Fail any in-flight requests
    for (auto& [easy, ifr] : in_flight_) {
        curl_multi_remove_handle(multi_, easy);
        HttpResponse resp;
        resp.curl_code = CURLE_ABORTED_BY_CALLBACK;
        if (ifr->slot) {
            ifr->slot->signal(std::move(resp));
        } else {
            ifr->promise.set_value(std::move(resp));
        }
        if (ifr->header_list) curl_slist_free_all(ifr->header_list);
        release_handle(easy);
    }
    in_flight_.clear();

    // Clean up handle pool
    for (auto* h : handle_pool_) curl_easy_cleanup(h);
    handle_pool_.clear();

    // Clean up DNS resolve list
    if (resolve_list_) {
        curl_slist_free_all(resolve_list_);
        resolve_list_ = nullptr;
    }

    if (multi_) {
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
    }

    if (heartbeat_fd_ >= 0) {
        close(heartbeat_fd_);
        heartbeat_fd_ = -1;
    }
    if (timer_fd_ >= 0) {
        close(timer_fd_);
        timer_fd_ = -1;
    }
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

void CurlMultiManager::wake_io_thread() {
    if (!worker_awake_.exchange(true, std::memory_order_acq_rel)) {
        uint64_t val = 1;
        if (::write(wakeup_fd_, &val, sizeof(val)) < 0) {
        }
    }
}

std::future<HttpResponse> CurlMultiManager::submit(HttpRequest req) {
    PendingEntry entry;
    entry.request = std::move(req);
    auto future = entry.promise.get_future();

    pending_queue_.enqueue(std::move(entry));
    wake_io_thread();

    return future;
}

std::pair<int, std::string> CurlMultiManager::execute(HttpRequest req) {
    CompletionSlot slot;

    PendingEntry entry;
    entry.request = std::move(req);
    entry.slot = &slot;

    pending_queue_.enqueue(std::move(entry));
    wake_io_thread();

    auto& resp = slot.wait();
    return {resp.status_code, std::move(resp.body)};
}

void CurlMultiManager::add_dns_pinning(const std::string& host, int port,
                                        const std::string& ip) {
    std::string entry = host + ":" + std::to_string(port) + ":" + ip;
    resolve_list_ = curl_slist_append(resolve_list_, entry.c_str());
    std::cout << "[CurlMultiManager] DNS pinned: " << entry << std::endl;
}

void CurlMultiManager::start_heartbeat(const std::string& url,
                                        int interval_sec) {
    heartbeat_url_ = url;

    struct itimerspec its {};
    its.it_value.tv_sec = interval_sec;
    its.it_interval.tv_sec = interval_sec;
    timerfd_settime(heartbeat_fd_, 0, &its, nullptr);

    std::cout << "[CurlMultiManager] Heartbeat started: every " << interval_sec
              << "s to " << url << std::endl;
}

void CurlMultiManager::warmup(const std::string& url) {
    // Submit concurrent requests to pre-establish connections.
    // HTTP/1.1 naturally opens separate connections for each request.
    constexpr int WARMUP_CONNECTIONS = 128;
    std::vector<std::future<HttpResponse>> futures;
    futures.reserve(WARMUP_CONNECTIONS);

    for (int i = 0; i < WARMUP_CONNECTIONS; i++) {
        HttpRequest req;
        req.url = url;
        req.method = "GET";
        req.timeout_ms = 10000;
        req.connect_timeout_ms = 5000;
        req.headers.push_back("Accept: */*");
        req.headers.push_back("Connection: keep-alive");
        futures.push_back(submit(std::move(req)));
    }

    for (auto& f : futures) {
        f.get();
    }
}

CURL* CurlMultiManager::acquire_handle() {
    if (!handle_pool_.empty()) {
        CURL* h = handle_pool_.back();
        handle_pool_.pop_back();
        curl_easy_reset(h);
        return h;
    }
    return curl_easy_init();
}

void CurlMultiManager::release_handle(CURL* h) {
    if (handle_pool_.size() < MAX_POOL_SIZE) {
        handle_pool_.push_back(h);
    } else {
        curl_easy_cleanup(h);
    }
}

void CurlMultiManager::fire_heartbeat() {
    if (heartbeat_url_.empty()) return;

    // Fire a single lightweight GET to keep connections alive
    HttpRequest req;
    req.url = heartbeat_url_;
    req.method = "GET";
    req.timeout_ms = 5000;
    req.connect_timeout_ms = 3000;
    req.headers.push_back("Accept: */*");
    req.headers.push_back("Connection: keep-alive");

    // Use submit() (async) so we don't block the event loop
    PendingEntry entry;
    entry.request = std::move(req);
    // No slot, no future needed — fire and forget via promise
    setup_easy_handle(entry);
}

void CurlMultiManager::event_loop() {
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (!shutting_down_.load(std::memory_order_relaxed)) {
        // Block indefinitely — eventfd wakeup ensures we never miss requests
        int nfds = epoll_wait(epoll_fd_, events, MAX_EPOLL_EVENTS, -1);

        // Mark worker as awake so submitters skip the eventfd write
        worker_awake_.store(true, std::memory_order_relaxed);

        // Always drain pending requests first to minimize queue latency
        process_pending_requests();

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == WAKEUP_SENTINEL) {
                // Drain eventfd
                uint64_t val;
                if (::read(wakeup_fd_, &val, sizeof(val)) < 0) {
                }
            } else if (events[i].data.ptr == TIMER_SENTINEL) {
                // Drain timerfd
                uint64_t expirations;
                if (::read(timer_fd_, &expirations, sizeof(expirations)) < 0) {
                }
                int running = 0;
                curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0,
                                         &running);
                check_multi_info();
            } else if (events[i].data.ptr == HEARTBEAT_SENTINEL) {
                // Drain heartbeat timerfd and fire keepalive
                uint64_t expirations;
                if (::read(heartbeat_fd_, &expirations, sizeof(expirations)) <
                    0) {
                }
                fire_heartbeat();
            } else {
                // Socket event from curl
                curl_socket_t sockfd = static_cast<curl_socket_t>(
                    reinterpret_cast<intptr_t>(events[i].data.ptr));
                int ev_bitmask = 0;
                if (events[i].events & EPOLLIN) ev_bitmask |= CURL_CSELECT_IN;
                if (events[i].events & EPOLLOUT)
                    ev_bitmask |= CURL_CSELECT_OUT;
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                    ev_bitmask |= CURL_CSELECT_ERR;

                int running = 0;
                curl_multi_socket_action(multi_, sockfd, ev_bitmask, &running);
                check_multi_info();
            }
        }

        // About to sleep — allow submitters to wake us again
        worker_awake_.store(false, std::memory_order_release);

        // Drain any requests that arrived while we were processing events
        // but before we cleared worker_awake_. This closes the race window.
        process_pending_requests();
    }
}

void CurlMultiManager::process_pending_requests() {
    PendingEntry entry;
    while (pending_queue_.try_dequeue(entry)) {
        setup_easy_handle(entry);
    }
}

void CurlMultiManager::setup_easy_handle(PendingEntry& entry) {
    CURL* easy = acquire_handle();
    if (!easy) {
        HttpResponse resp;
        resp.curl_code = CURLE_FAILED_INIT;
        if (entry.slot) {
            entry.slot->signal(std::move(resp));
        } else {
            entry.promise.set_value(std::move(resp));
        }
        return;
    }

    auto ifr = std::make_unique<InFlightRequest>();
    ifr->easy = easy;
    ifr->promise = std::move(entry.promise);
    ifr->slot = entry.slot;

    // URL
    curl_easy_setopt(easy, CURLOPT_URL, entry.request.url.c_str());

    // Method + body
    const auto& method = entry.request.method;
    if (method == "POST") {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS,
                         entry.request.body.c_str());
    } else if (method == "PUT") {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS,
                         entry.request.body.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!entry.request.body.empty()) {
            curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS,
                             entry.request.body.c_str());
        }
    }
    // GET is the default

    // Headers
    struct curl_slist* header_list = nullptr;
    for (const auto& h : entry.request.headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, header_list);
    }
    ifr->header_list = header_list;

    // Write callback
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ifr->response_body);

    // Timeouts
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, entry.request.timeout_ms);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                     entry.request.connect_timeout_ms);

    // HTTP/1.1 — avoids HTTP/2 multiplexing HOL blocking
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_TCP_NODELAY, 1L);

    // DNS pinning — bypass getaddrinfo entirely on hot path
    if (resolve_list_) {
        curl_easy_setopt(easy, CURLOPT_RESOLVE, resolve_list_);
    }

    // DNS caching fallback and buffer tuning
    curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256 * 1024L);

    // TCP keepalive to detect dead connections early
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, 15L);

    // Store pointer for retrieval in check_multi_info
    curl_easy_setopt(easy, CURLOPT_PRIVATE, ifr.get());

    // Track and add to multi
    in_flight_[easy] = std::move(ifr);

    CURLMcode mc = curl_multi_add_handle(multi_, easy);
    if (mc != CURLM_OK) {
        // Failed to add — fulfill promise/slot with error
        auto it = in_flight_.find(easy);
        if (it != in_flight_.end()) {
            HttpResponse resp;
            resp.curl_code = CURLE_FAILED_INIT;
            if (it->second->slot) {
                it->second->slot->signal(std::move(resp));
            } else {
                it->second->promise.set_value(std::move(resp));
            }
            if (it->second->header_list)
                curl_slist_free_all(it->second->header_list);
            release_handle(easy);
            in_flight_.erase(it);
        }
    }
}

void CurlMultiManager::check_multi_info() {
    CURLMsg* msg;
    int msgs_left;

    while ((msg = curl_multi_info_read(multi_, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        CURLcode result = msg->data.result;

        auto it = in_flight_.find(easy);
        if (it == in_flight_.end()) continue;

        auto& ifr = it->second;

        HttpResponse resp;
        resp.curl_code = result;

        if (result == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
            resp.status_code = static_cast<int>(http_code);
            resp.body = std::move(ifr->response_body);
        }

        if (ifr->slot) {
            ifr->slot->signal(std::move(resp));
        } else {
            ifr->promise.set_value(std::move(resp));
        }

        // Cleanup
        curl_multi_remove_handle(multi_, easy);
        if (ifr->header_list) curl_slist_free_all(ifr->header_list);
        release_handle(easy);
        in_flight_.erase(it);
    }
}

int CurlMultiManager::socket_callback(CURL* easy, curl_socket_t s, int what,
                                      void* userp, void* socketp) {
    auto* self = static_cast<CurlMultiManager*>(userp);

    if (what == CURL_POLL_REMOVE) {
        epoll_ctl(self->epoll_fd_, EPOLL_CTL_DEL, s, nullptr);
        return 0;
    }

    struct epoll_event ev {};
    // Store the socket fd as the data pointer (cast to void*)
    ev.data.ptr = reinterpret_cast<void*>(static_cast<intptr_t>(s));

    if (what & CURL_POLL_IN) ev.events |= EPOLLIN;
    if (what & CURL_POLL_OUT) ev.events |= EPOLLOUT;

    // Try MOD first; if it fails (socket not yet added), use ADD
    if (epoll_ctl(self->epoll_fd_, EPOLL_CTL_MOD, s, &ev) != 0) {
        epoll_ctl(self->epoll_fd_, EPOLL_CTL_ADD, s, &ev);
    }

    return 0;
}

int CurlMultiManager::timer_callback(CURLM* multi, long timeout_ms,
                                     void* userp) {
    auto* self = static_cast<CurlMultiManager*>(userp);

    struct itimerspec its {};
    if (timeout_ms > 0) {
        its.it_value.tv_sec = timeout_ms / 1000;
        its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000L;
    } else if (timeout_ms == 0) {
        // Expire immediately
        its.it_value.tv_nsec = 1;
    }
    // timeout_ms == -1 → disarm (its is already zero)

    timerfd_settime(self->timer_fd_, 0, &its, nullptr);
    return 0;
}

size_t CurlMultiManager::write_callback(char* ptr, size_t size, size_t nmemb,
                                        void* userdata) {
    size_t total = size * nmemb;
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, total);
    return total;
}
