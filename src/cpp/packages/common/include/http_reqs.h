#pragma once

#include <curl/curl.h>

#include <cstdio>
#include <string>

namespace HttpUtils {
size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb,
                         void* userp);
size_t DiscardOutput(char* ptr, size_t size, size_t nmemb, void* userdata);
}  // namespace HttpUtils
