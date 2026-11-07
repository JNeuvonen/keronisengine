#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

class RotatingLog {
    std::ofstream file_;
    std::ofstream error_file_;
    std::filesystem::path base_path_;
    std::filesystem::path error_base_path_;
    const std::size_t max_bytes_;
    std::size_t current_bytes_{0};
    std::size_t current_error_bytes_{0};
    std::mutex mutex_;
    std::mutex error_mutex_;
    const std::string newline_ = "\n";

    static std::string timestamp_dir_name() {
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_MSC_VER)
        gmtime_s(&tm, &now_c);
#else
        gmtime_r(&now_c, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%y-%m-%d_%H-%M");
        return oss.str();
    }

    static std::filesystem::path resolve_log_path(bool is_error = false) {
        const char* app_name = std::getenv("APPLICATION_NAME");
        const char* log_dir = std::getenv("LOG_DIRECTORY");

        if (!app_name || !*app_name) {
            throw std::runtime_error(
                "Missing required environment variable: APPLICATION_NAME");
        }

        if (!log_dir || !*log_dir) {
            throw std::runtime_error(
                "Missing required environment variable: LOG_DIRECTORY");
        }

        return std::filesystem::path(log_dir) / app_name /
               timestamp_dir_name() / (is_error ? "error_log.txt" : "log.txt");
    }

    void open_new_file(bool is_error = false) {
        const auto path = is_error ? error_base_path_ : base_path_;
        const auto dir = path.parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }

        auto& file = is_error ? error_file_ : file_;
        file.open(path, std::ios::out | std::ios::trunc);
        if (!file) {
            throw std::runtime_error(
                "RotatingLog: failed to open log file at " + path.string());
        }

        if (is_error) {
            current_error_bytes_ = 0;
        } else {
            current_bytes_ = 0;
        }
    }

    void rotate(bool is_error = false) {
        auto& file = is_error ? error_file_ : file_;
        if (file.is_open()) file.close();

        const auto timestamp = [] {
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_MSC_VER)
            gmtime_s(&tm, &now_c);
#else
            gmtime_r(&now_c, &tm);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
            return oss.str();
        }();

        const auto base_path = is_error ? error_base_path_ : base_path_;
        auto rotated_name =
            base_path.parent_path() /
            ((is_error ? "error_log_" : "log_") + timestamp + ".log");

        if (std::filesystem::exists(base_path)) {
            std::filesystem::rename(base_path, rotated_name);
        }

        open_new_file(is_error);
    }

   public:
    explicit RotatingLog(std::size_t max_size)
        : max_bytes_(max_size),
          base_path_(resolve_log_path()),
          error_base_path_(resolve_log_path(true)) {
        open_new_file();
        open_new_file(true);
    }

    ~RotatingLog() {
        try {
            if (file_.is_open()) file_.flush();
            if (error_file_.is_open()) error_file_.flush();
        } catch (...) {
        }
    }

    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << msg << "\n";

        if (current_bytes_ + msg.size() + newline_.size() > max_bytes_) {
            rotate();
        }

        file_ << msg << newline_;
        file_.flush();
        current_bytes_ += msg.size() + newline_.size();
    }

    void log_error(const std::string& msg) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        std::cerr << msg << "\n";

        if (current_error_bytes_ + msg.size() + newline_.size() > max_bytes_) {
            rotate(true);
        }

        error_file_ << msg << newline_;
        error_file_.flush();
        current_error_bytes_ += msg.size() + newline_.size();
    }

    RotatingLog(const RotatingLog&) = delete;
    RotatingLog& operator=(const RotatingLog&) = delete;
};