#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace CommonUtils {

// Write `j` to `path` atomically via tmp-and-rename. Any existing file at
// `path` is replaced only after the tmp is fully written, so a crash mid-
// write leaves the previous good file intact.
void atomic_write_json(const std::filesystem::path& path,
                       const nlohmann::json& j, int indent = 2);

// Append `j` as a single line to `path`. Opens in append mode; not
// safe across multiple processes writing the same file.
void append_jsonl(const std::filesystem::path& path,
                  const nlohmann::json& j);

// Read and parse JSON from `path`. Returns nullopt if the file is missing.
// Throws on parse errors.
std::optional<nlohmann::json> read_json_if_exists(
    const std::filesystem::path& path);

}  // namespace CommonUtils
