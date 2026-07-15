#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace aiSDK {

// ToolSchema 先直接保存 JSON Schema，
// 这样后续对接 DeepSeek 和 MiniMax 时可以共用一份定义。
struct ToolSchema {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object();
};

}  // namespace aiSDK