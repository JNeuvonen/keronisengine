#include <catch2/catch_all.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "curl_multi_manager.h"

static const std::string CLOB_HOST = "https://clob.polymarket.com";
static const std::string TEST_ENDPOINT = CLOB_HOST + "/time";

// Resolve hostname to first IPv4 address via getaddrinfo
static std::string resolve_ipv4(const std::string& hostname) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    REQUIRE(rc == 0);
    REQUIRE(result != nullptr);

    std::array<char, INET_ADDRSTRLEN> ip_buf{};
    auto* addr =
        reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    inet_ntop(AF_INET, &addr->sin_addr, ip_buf.data(),
              static_cast<socklen_t>(ip_buf.size()));

    std::string ip(ip_buf.data());
    freeaddrinfo(result);
    return ip;
}

TEST_CASE("DNS pinning routes to pinned IP",
          "[http_engine_jitter_patch][live]") {
    auto& mgr = CurlMultiManager::instance();

    std::string ip = resolve_ipv4("clob.polymarket.com");
    REQUIRE(!ip.empty());

    mgr.add_dns_pinning("clob.polymarket.com", 443, ip);

    HttpRequest req;
    req.url = TEST_ENDPOINT;
    req.method = "GET";
    req.timeout_ms = 10000;
    req.connect_timeout_ms = 5000;

    auto [status, body] = mgr.execute(req);
    REQUIRE(status == 200);
    REQUIRE(!body.empty());
}

TEST_CASE("execute returns valid response",
          "[http_engine_jitter_patch][live]") {
    auto& mgr = CurlMultiManager::instance();

    HttpRequest req;
    req.url = TEST_ENDPOINT;
    req.method = "GET";
    req.timeout_ms = 10000;
    req.connect_timeout_ms = 5000;

    auto [status, body] = mgr.execute(req);
    REQUIRE(status == 200);
    REQUIRE(!body.empty());
}

TEST_CASE("submit returns future that resolves",
          "[http_engine_jitter_patch][live]") {
    auto& mgr = CurlMultiManager::instance();

    HttpRequest req;
    req.url = TEST_ENDPOINT;
    req.method = "GET";
    req.timeout_ms = 10000;
    req.connect_timeout_ms = 5000;

    auto fut = mgr.submit(req);
    HttpResponse resp = fut.get();
    REQUIRE(resp.status_code == 200);
    REQUIRE(!resp.body.empty());
}

TEST_CASE("concurrent requests complete independently",
          "[http_engine_jitter_patch][live]") {
    auto& mgr = CurlMultiManager::instance();

    constexpr int N = 8;
    std::vector<std::future<HttpResponse>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i) {
        HttpRequest req;
        req.url = TEST_ENDPOINT;
        req.method = "GET";
        req.timeout_ms = 15000;
        req.connect_timeout_ms = 5000;
        futures.push_back(mgr.submit(req));
    }

    for (auto& fut : futures) {
        HttpResponse resp = fut.get();
        REQUIRE(resp.status_code == 200);
        REQUIRE(!resp.body.empty());
    }
}

// Regression: CompletionSlot::wait() used to spin on std::this_thread::yield()
// and pinned one core per in-flight sync call. This test fans out 32 parallel
// execute() calls and verifies total user-CPU is well below wall_clock ×
// concurrency. With the busy-wait bug, 32 × ~100ms requests burn ~3200 ms of
// user CPU. With the futex-backed fix, total user CPU is ~a few ms.
TEST_CASE("sync execute does not burn CPU while waiting",
          "[http_engine_jitter_patch][curl_multi][cpu][live]") {
    auto& mgr = CurlMultiManager::instance();

    auto user_cpu_ms = []() -> long {
        struct rusage ru {};
        getrusage(RUSAGE_SELF, &ru);
        return static_cast<long>(ru.ru_utime.tv_sec) * 1000 +
               static_cast<long>(ru.ru_utime.tv_usec) / 1000;
    };

    // Warm curl + DNS so the measurement reflects the wait path, not
    // connection setup.
    {
        HttpRequest warm;
        warm.url = TEST_ENDPOINT;
        warm.method = "GET";
        warm.timeout_ms = 10000;
        warm.connect_timeout_ms = 5000;
        (void)mgr.execute(warm);
    }

    constexpr int kConcurrency = 32;
    std::atomic<int> ok_count{0};

    const auto wall_start = std::chrono::steady_clock::now();
    const long cpu_start = user_cpu_ms();

    std::vector<std::thread> workers;
    workers.reserve(kConcurrency);
    for (int i = 0; i < kConcurrency; ++i) {
        workers.emplace_back([&]() {
            HttpRequest req;
            req.url = TEST_ENDPOINT;
            req.method = "GET";
            req.timeout_ms = 15000;
            req.connect_timeout_ms = 5000;
            auto [status, body] = mgr.execute(req);
            if (status == 200 && !body.empty()) {
                ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : workers) t.join();

    const long cpu_end = user_cpu_ms();
    const auto wall_end = std::chrono::steady_clock::now();

    const long wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             wall_end - wall_start)
                             .count();
    const long cpu_ms = cpu_end - cpu_start;

    INFO("kConcurrency=" << kConcurrency << " wall_ms=" << wall_ms
                         << " user_cpu_ms=" << cpu_ms
                         << " ok_count=" << ok_count.load());

    REQUIRE(ok_count.load() == kConcurrency);

    // Busy-wait would produce cpu_ms ≈ wall_ms × kConcurrency. Allow a
    // generous 4× wall_ms ceiling: the parked-in-kernel fix lands well
    // under 1× wall_ms in practice.
    REQUIRE(cpu_ms < wall_ms * 4);
}

TEST_CASE("heartbeat can be started without crash",
          "[http_engine_jitter_patch]") {
    auto& mgr = CurlMultiManager::instance();

    mgr.start_heartbeat(TEST_ENDPOINT, 15);

    // Smoke test: just verify no crash after a brief pause
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SUCCEED("heartbeat started without crash");
}
