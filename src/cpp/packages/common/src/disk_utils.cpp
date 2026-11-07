#include "disk_utils.h"

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace DiskUtils {

long long get_free_disk_space_mb(const std::filesystem::path& path) {
#ifdef _WIN32
    ULARGE_INTEGER free_bytes_available;
    if (!GetDiskFreeSpaceExW(path.c_str(), &free_bytes_available, nullptr,
                             nullptr)) {
        return 0;
    }
    return static_cast<long long>(free_bytes_available.QuadPart) /
           (1024 * 1024);
#else
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return 0;
    }
    return (static_cast<long long>(stat.f_bavail) *
            static_cast<long long>(stat.f_frsize)) /
           (1024 * 1024);
#endif
}

}  // namespace DiskUtils