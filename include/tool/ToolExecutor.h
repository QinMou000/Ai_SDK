#pragma once

#include <vector>

#include "core/Message.h"
#include "tool/ToolCall.h"
#include "tool/ToolRegistry.h"
#include "trace/TraceRecorder.h"

namespace aiSDK {

struct ToolExecutionResult {
    // call 保留模型原始调用标识和参数，便于结果与请求一一关联。
    ToolCall call;
    // result 保存本地处理函数的结构化成功值或失败原因。
    ToolResult result;

    // toToolMessage 把执行结果转换为带 tool_call_id 的 Tool 角色消息。
    // 成功内容序列化为 JSON，失败内容使用 error_message；
    // 该方法只完成值对象转换，不修改历史，也不触发模型请求。
    Message toToolMessage() const;
};

// ToolExecutor 负责执行一批已经由 Provider 解析完成的 ToolCall。
// 第一阶段保持同步串行，确保执行顺序与模型输出一致；
// 并发调度、权限审批和重试策略属于更高层能力，不在这里隐式发生。
class ToolExecutor {
   public:
    // registry 由调用方持有，执行器只借用其生命周期，不取得所有权。
    explicit ToolExecutor(ToolRegistry& registry);

    // executeAll 严格保持模型返回顺序，并为每个输入生成一个结果。
    // ToolRegistry 会把未知工具和处理函数异常转成失败结果，
    // 因此单个失败不会中止同一批次中的后续调用。
    std::vector<ToolExecutionResult> executeAll(const std::vector<ToolCall>& calls);
    // Trace 重载创建一个批次根步骤，并为每个 ToolCall 创建显式子步骤。
    // TraceSession 线程安全不代表 ToolRegistry 线程安全，注册表并发仍由调用方协调。
    std::vector<ToolExecutionResult> executeAll(const std::vector<ToolCall>& calls, TraceSession& trace_session);

   private:
    // registry_ 是非拥有引用，生命周期必须长于当前 ToolExecutor。
    ToolRegistry& registry_;
};

}  // namespace aiSDK
