#include "file.h"

#include <fstream>

void CommonUtils::write_str_to_disk(const std::string& path,
                                    const std::string& str) {
    std::ofstream outFile(path, std::ios::out | std::ios::trunc);
    if (!outFile) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    outFile << str;
    outFile.close();
}

std::string CommonUtils::read_file(const std::string& path) {
    std::ifstream inFile(path, std::ios::in);
    if (!inFile) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    std::string str((std::istreambuf_iterator<char>(inFile)),
                    std::istreambuf_iterator<char>());
    return str;
}
