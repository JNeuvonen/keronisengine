#include "ip_rotator.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

// ROTATE_IP=1 rotates outbound requests across the host's own addresses,
// listed comma-separated in ROTATE_IP_LIST.
IPRotator::IPRotator() {
    const char* rotate_ip_env = std::getenv("ROTATE_IP");
    enabled_ = rotate_ip_env && std::string(rotate_ip_env) == "1";
    if (const char* list = std::getenv("ROTATE_IP_LIST")) {
        std::stringstream ss(list);
        for (std::string ip; std::getline(ss, ip, ',');) {
            if (!ip.empty()) ip_list_.push_back(ip);
        }
    }
}

IPRotator& IPRotator::getInstance() {
    static IPRotator instance;
    return instance;
}

std::string IPRotator::getNextIP() {
    if (!enabled_ || ip_list_.empty()) {
        return "";
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "getNextIP: enabled_ = " << enabled_
              << ", ip_list_.size() = " << ip_list_.size() << std::endl;

    size_t index = current_index_.load();
    std::string ip = ip_list_[index];

    current_index_.store((index + 1) % ip_list_.size());
    return ip;
}
