#pragma once
#include <string>

namespace Core {

struct MintContractsRequest {
    std::string condition_id;
    bool neg_risk = false;
    int quantity;
};

}  // namespace Core
