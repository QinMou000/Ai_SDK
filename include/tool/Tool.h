#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace aiSDK {

// ToolRiskLevel 只描述工具的静态风险等级。
// 当前 SDK 负责保存该元数据，不在执行阶段自动审批或拦截。
enum class ToolRiskLevel {
    // Low 表示只读或影响范围可忽略的工具。
    Low,
    // Medium 表示可能修改局部状态、需要上层关注的工具。
    Medium,
    // High 表示可能产生外部副作用，应由上层应用决定是否执行。
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

    // successResult 接收任意 JSON 值并生成成功结果；值按值传入，
    // 便于调用方安全移动临时对象而不保留外部引用。
    static ToolResult successResult(nlohmann::json result);
    // errorResult 生成不携带伪造 data 的失败结果，错误文本应可直接用于诊断。
    static ToolResult errorResult(std::string message);
};

// ToolHandler 是本地 C++ 工具的同步执行协议。
// 参数来自模型输出，处理函数必须把业务校验失败表达为 ToolResult；
// 如果处理函数仍抛出异常，ToolRegistry 会在扩展边界统一收敛。
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
