#include "str_utils.h"

namespace StrUtils {

std::vector<std::string> split_str(const std::string& str,
                                   const std::string& delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, end = 0;
    while ((end = str.find(delimiter, start)) != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
    }
    tokens.push_back(str.substr(start));
    return tokens;
}
}  // namespace StrUtils