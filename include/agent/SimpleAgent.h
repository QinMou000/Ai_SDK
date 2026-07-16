#pragma once

#include <functional>
#include <optional>
#include <string>

#include "AIClient.h"
#include "agent/WorkspaceFileTools.h"

namespace aiSDK {

// AgentResult 只承载一次独立任务的最终状态，
// 不保存跨任务消息，后续会话和记忆能力可在该稳定结果边界之外扩展。
struct AgentResult {
    bool success = false;
    std::string final_answer;
    std::string error_message;
};

// AgentStreamEventType 表达流式 Agent 对调用方可见的最小生命周期。
// 工具参数和工具结果正文仍只回填给模型，避免回调被默认用作敏感数据旁路。
enum class AgentStreamEventType { TextDelta, ToolCallReady, ToolExecutionFinished };

// AgentStreamEvent 在同步 runStream 调用期间按发生顺序交付。
// TextDelta 使用 delta；工具事件使用 tool_call_id、tool_name 与 success。
struct AgentStreamEvent {
    AgentStreamEventType type = AgentStreamEventType::TextDelta;
    std::string delta;
    std::string tool_call_id;
    std::string tool_name;
    bool success = false;
};

using AgentStreamCallback = std::function<void(const AgentStreamEvent&)>;

// SimpleAgentOptions 把与单个实例相关的策略集中在构造阶段。
// 其中工作区工具是可选能力；未提供根目录时 Agent 不会注册任何文件工具。
struct SimpleAgentOptions {
    std::string system_prompt = "你是一个简洁的中文助手。需要外部信息或执行工作区操作时，请使用提供的工具。";
    std::optional<WorkspaceFileToolOptions> workspace_file_tools;
};

// SimpleAgent 是位于 AIClient 之上的最小 ReAct 编排层。
// 它只组织模型调用、工具结果回填和终止条件，不改变 AIClient 的 Provider 或传输职责。
class SimpleAgent {
   public:
    explicit SimpleAgent(AIClient& client, SimpleAgentOptions options = {});

    // run 执行一次独立任务；模型不再返回 Tool Call 时自然结束。
    // 该重载不记录 Trace，调用方如需观察完整过程应使用显式会话重载。
    AgentResult run(const std::string& input);

    // Trace 重载复用调用方会话，让多轮模型请求和工具批次进入同一时间线。
    // 该方法不创建隐式会话，也不改变 AIClient 已有的 Trace 根步骤层级。
    AgentResult run(const std::string& input, TraceSession& trace_session);

    // runStream 在当前线程中持续交付文本与工具状态；模型流结束后才聚合并执行 Tool Call。
    // callback 不能为空，回调抛出的异常会按 Agent 运行期失败收敛为 AgentResult。
    AgentResult runStream(const std::string& input, AgentStreamCallback callback);

    // Trace 重载使流式模型请求、工具批次和后续模型请求写入同一调用方会话。
    AgentResult runStream(const std::string& input, AgentStreamCallback callback, TraceSession& trace_session);

   private:
    // 内部指针只用于复用两种公开重载的循环实现；其生命周期始终受同步 run 调用约束。
    // 该私有入口不接受工具或消息历史参数，防止调用方绕过单任务隔离契约。
    AgentResult runInternal(const std::string& input, TraceSession* trace_session);
    AgentResult runStreamInternal(const std::string& input, AgentStreamCallback callback, TraceSession* trace_session);

    AIClient& client_;
    std::string system_prompt_;
};

}  // namespace aiSDK
