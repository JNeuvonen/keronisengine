#pragma once

#include <datetime_utils.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

// global_trade_lock.h
class FileLock {
   public:
    static bool has_lock;  // Declaration only

    FileLock(const std::string& path = "/tmp/trading.lock")
        : lock_path(path), fd(-1), first_run(false) {
        std::string attempts_path = lock_path + ".attempts";

        struct stat buffer;
        if (stat(attempts_path.c_str(), &buffer) != 0) {
            first_run = true;
        }

        int attempts_fd = open(attempts_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (attempts_fd == -1) {
            std::cerr << "Failed to create attempts file: " << strerror(errno)
                      << std::endl;
        } else {
            close(attempts_fd);
        }

        fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd == -1) {
            throw std::runtime_error("Failed to create lock file: " +
                                     std::string(strerror(errno)));
        }
    }

    ~FileLock() {
        if (fd != -1) {
            close(fd);
        }
    }

    bool try_lock() {
        struct flock fl;
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;

        int result = fcntl(fd, F_SETLK, &fl);
        if (result == -1) {
            if (errno == EACCES || errno == EAGAIN) {
                // Lock is held by another process
                return false;
            }
            throw std::runtime_error("Lock operation failed: " +
                                     std::string(strerror(errno)));
        }

        // Write PID and timestamp to lock file for debugging
        std::ostringstream info;
        info << "PID: " << getpid()
             << " Time: " << DatetimeUtils::get_current_time_ms() << "\n";

        std::string info_str = info.str();
        int tr = ftruncate(fd, 0);
        (void)tr;
        ssize_t wr = write(fd, info_str.c_str(), info_str.length());
        (void)wr;

        return true;
    }

    void unlock() {
        struct flock fl;
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;

        if (fcntl(fd, F_SETLK, &fl) == -1) {
            // Log error but don't throw in destructor path
            std::cerr << "Failed to unlock file: " << strerror(errno)
                      << std::endl;
        }
    }

    bool is_first_run() const { return first_run; }

    // RAII guard
    class ScopedLock {
       public:
        ScopedLock(FileLock& lock) : lock_(lock), is_locked_(false) {
            is_locked_ = lock_.try_lock();
            FileLock::has_lock = is_locked_;
        }

        ~ScopedLock() {
            if (is_locked_) {
                lock_.unlock();
                FileLock::has_lock = false;
            }
        }

        bool is_locked() const { return is_locked_; }
        void unlock() {
            if (is_locked_) {
                lock_.unlock();
                FileLock::has_lock = false;
            }
        }

       private:
        FileLock& lock_;
        bool is_locked_;

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
    };

   private:
    std::string lock_path;
    bool first_run;
    int fd;

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};