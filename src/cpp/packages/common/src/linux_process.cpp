#include "linux_process.h"

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

namespace LinuxProcess {

bool set_process_priority(int nice_value) {
#ifdef __linux__
    if (setpriority(PRIO_PROCESS, 0, nice_value) == 0) {
        return true;
    }

    int priority_errno = errno;

    errno = 0;
    int nice_result = nice(nice_value);
    if (nice_result != -1 || errno == 0) {
        return true;
    }

    errno = priority_errno;
    return false;
#else
    return true;
#endif
}

int64_t read_rss_bytes() {
#ifdef __linux__
    long pages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long total_pages = 0;
    int scanned = std::fscanf(f, "%ld %ld", &total_pages, &pages);
    std::fclose(f);
    if (scanned < 2) return 0;
    return static_cast<int64_t>(pages) *
           static_cast<int64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

}  // namespace LinuxProcess