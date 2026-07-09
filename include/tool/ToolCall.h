#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace aiSDK {

// ToolCall 是模型输出和工具执行之间的统一协议对象。
// raw_arguments 会在后续调试模型返回格式差异时直接复用。
struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json arguments = nlohmann::json::object();
    std::string raw_arguments;
};

}  // namespace aiSDK