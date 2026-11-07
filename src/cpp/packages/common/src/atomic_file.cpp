#include "atomic_file.h"

#include <fstream>
#include <stdexcept>

namespace CommonUtils {

void atomic_write_json(const std::filesystem::path& path,
                       const nlohmann::json& j, int indent) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("atomic_write_json: failed to open " +
                                     tmp.string());
        }
        out << j.dump(indent);
        if (!out) {
            throw std::runtime_error("atomic_write_json: write failed for " +
                                     tmp.string());
        }
    }

    std::filesystem::rename(tmp, path);
}

void append_jsonl(const std::filesystem::path& path, const nlohmann::json& j) {
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        throw std::runtime_error("append_jsonl: failed to open " +
                                 path.string());
    }
    out << j.dump() << "\n";
    if (!out) {
        throw std::runtime_error("append_jsonl: write failed for " +
                                 path.string());
    }
}

std::optional<nlohmann::json> read_json_if_exists(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("read_json_if_exists: failed to open " +
                                 path.string());
    }
    nlohmann::json j;
    in >> j;
    return j;
}

}  // namespace CommonUtils
