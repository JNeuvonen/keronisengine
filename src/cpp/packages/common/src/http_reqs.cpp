#include <http_reqs.h>

size_t HttpUtils::CurlWriteCallback(void* contents, size_t size, size_t nmemb,
                                    void* userp) {
    size_t totalSize = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

size_t HttpUtils::DiscardOutput(char* ptr, size_t size, size_t nmemb,
                                void* userdata) {
    return size * nmemb;
}
