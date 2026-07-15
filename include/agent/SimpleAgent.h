#pragma once

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

   private:
    // 内部指针只用于复用两种公开重载的循环实现；其生命周期始终受同步 run 调用约束。
    // 该私有入口不接受工具或消息历史参数，防止调用方绕过单任务隔离契约。
    AgentResult runInternal(const std::string& input, TraceSession* trace_session);

    AIClient& client_;
    std::string system_prompt_;
};

}  // namespace aiSDK
