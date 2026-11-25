#pragma once

#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sdvx {

struct Selection;
inline constexpr int AutomationParadise2019 = 1438;

void prepareAutomationParadise(const Selection&, nlohmann::json& chart, std::string& wave,
                               std::vector<int16_t>& dryPcm, uint32_t seed);

} // namespace sdvx
