#pragma once

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>

namespace LinuxProcess {

/**
 * Sets the process priority using nice value.
 * @param nice_value Range from 19 (lowest priority) to -20 (highest priority,
 * requires root)
 * @return true if priority was set successfully, false otherwise
 */
bool set_process_priority(int nice_value);

// /proc/self/statm resident pages * page size. Returns 0 on failure / non-linux.
int64_t read_rss_bytes();

}  // namespace LinuxProcess
