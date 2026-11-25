#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace sdvx {

nlohmann::json parseVox(std::string_view bytes, const nlohmann::json& judgment, int maxExscore);

} // namespace sdvx
