#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class IPRotator {
   public:
    static IPRotator& getInstance();

    std::string getNextIP();
    bool isEnabled() const { return enabled_; }

    // Delete copy constructor and assignment operator
    IPRotator(const IPRotator&) = delete;
    IPRotator& operator=(const IPRotator&) = delete;

   private:
    IPRotator();

    std::vector<std::string> ip_list_;
    std::atomic<size_t> current_index_{0};
    bool enabled_{false};
    mutable std::mutex mutex_;
};
