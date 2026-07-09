#pragma once

#include <functional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace aiSDK {

enum class ToolRiskLevel {
    Low,
    Medium,
    High
};

// ToolResult 统一工具执行结果格式，
// 成功和失败都走一个出口，便于后续 Trace 记录。
struct ToolResult {
    // success 标记工具执行是否成功。
    bool success = false;
    // data 保存成功时的结构化结果。
    nlohmann::json data = nlohmann::json::object();
    // error_message 保存失败原因，供模型回填或日志记录。
    std::string error_message;

    // successResult / errorResult 保证成功和失败都走统一构造入口，
    // 这样 ToolExecutor 和测试不需要手写字段组合。
    static ToolResult successResult(nlohmann::json result) {
        return ToolResult{true, std::move(result), ""};
    }

    static ToolResult errorResult(std::string message) {
        return ToolResult{false, nlohmann::json::object(), std::move(message)};
    }
};

using ToolHandler = std::function<ToolResult(const nlohmann::json& arguments)>;

// Tool 保存模型可见的工具元数据。
// 真正的执行逻辑由 ToolRegistry 维护的 handler 承载。
struct Tool {
    // name 是工具的稳定标识，也会被模型在 tool_call 中引用。
    std::string name;
    // description 提供给模型理解工具用途。
    std::string description;
    // parameters 保存工具的 JSON Schema 描述。
    nlohmann::json parameters = nlohmann::json::object();
    // risk_level 为后续高风险工具拦截和审批预留。
    ToolRiskLevel risk_level = ToolRiskLevel::Low;
};

}  // namespace aiSDK
