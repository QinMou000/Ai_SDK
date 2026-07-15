#include "agent/SimpleAgent.h"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Message.h"

namespace aiSDK {
namespace {

constexpr std::size_t kMaxModelRequests = 16U;
// 该常量是服务端模型失控时的保护阈值，而不是调用方可调的业务轮次参数。
// 保持内部常量能防止不同调用点因传入不一致的预算得到难以审计的权限行为。

// lowRiskTools 每轮从注册表快照中筛选可见工具。
// 这样后续新增 Medium/High 工具不会自动扩大当前 Agent 的执行权限。
std::vector<Tool> lowRiskTools(const ToolRegistry& registry) {
    // listTools 返回值副本，因此本函数不会持有注册表内部容器的引用。
    // Agent 只依赖静态风险元数据，不尝试推断业务处理函数是否安全。
    std::vector<Tool> allowed_tools;
    for(const Tool& tool : registry.listTools()) {
        if(tool.risk_level == ToolRiskLevel::Low) {
            allowed_tools.push_back(tool);
        }
    }
    return allowed_tools;
}

// executeAllowedCalls 保留模型返回顺序，同时在 Agent 层二次拦截非 Low 工具。
// 未知工具继续交给现有 ToolExecutor 收敛，避免复制注册表已有的错误语义。
std::vector<ToolExecutionResult> executeAllowedCalls(AIClient& client, const std::vector<ToolCall>& calls, TraceSession* trace_session) {
    // 结果容器先按原调用数量构造，保证被拒绝和可执行调用可在同一索引空间合并。
    // 后续回填消息的顺序必须等同于模型 Tool Call 的原顺序，不能按风险分组重排。
    std::vector<ToolExecutionResult> results(calls.size());
    std::vector<ToolCall> executable_calls;
    std::vector<std::size_t> executable_indices;

    executable_calls.reserve(calls.size());
    executable_indices.reserve(calls.size());

    for(std::size_t index = 0; index < calls.size(); ++index) {
        const ToolCall& call = calls[index];
        // getTool 的异常只用于区分“已注册但禁止”与“名称未知”两条策略路径。
        // 未知调用仍交给既有执行器，避免 Agent 重新定义底层错误文本或 Trace 行为。
        try {
            const Tool tool = client.tools().getTool(call.name);
            if(tool.risk_level != ToolRiskLevel::Low) {
                // 风险等级由注册时声明；Agent 必须在真正执行前再次检查，
                // 防止模型绕过请求工具列表直接臆造中高风险工具名称。
                results[index] = ToolExecutionResult{
                    call,
                    ToolResult::errorResult("Agent 拒绝执行非低风险工具: " + call.name),
                };
                continue;
            }
        } catch(const std::out_of_range&) {
            // 未知名称由既有执行器生成标准失败 ToolResult，
            // 同时保留 Trace 中的工具批次，便于调用方定位模型输出问题。
        }

        executable_indices.push_back(index);
        executable_calls.push_back(call);
    }

    if(executable_calls.empty()) {
        // 全部调用被风险策略拒绝时不创建空 ToolBatch，返回的拒绝观察已足够驱动下一轮模型决策。
        return results;
    }

    const std::vector<ToolExecutionResult> executable_results =
        trace_session == nullptr ? client.executeToolCalls(executable_calls) : client.executeToolCalls(executable_calls, *trace_session);
    // executeToolCalls 保证输出数量与输入压缩批次一致；Agent 只负责将其还原到完整批次位置。
    // 若底层未来改变该契约，应由 ToolExecutor 层测试先发现，不能在此静默填充伪造结果。
    for(std::size_t index = 0; index < executable_indices.size(); ++index) {
        // executable_indices 将压缩批次的结果恢复到原始模型调用位置。
        results[executable_indices[index]] = executable_results[index];
    }
    return results;
}

}  // namespace

SimpleAgent::SimpleAgent(AIClient& client, SimpleAgentOptions options) : client_(client), system_prompt_(std::move(options.system_prompt)) {
    // 文件工具注册是可选构造行为；根目录缺失时保持纯模型与调用方自定义工具模式。
    if(options.workspace_file_tools.has_value()) {
        registerWorkspaceFileTools(client_.tools(), *options.workspace_file_tools);
    }
}

AgentResult SimpleAgent::run(const std::string& input) {
    // 普通入口显式传入空指针，确保不会因为默认构造 TraceSession 而产生隐式链路状态。
    return runInternal(input, nullptr);
}

AgentResult SimpleAgent::run(const std::string& input, TraceSession& trace_session) {
    // TraceSession 的所有权仍在调用方；Agent 仅在本次循环中借用该会话。
    return runInternal(input, &trace_session);
}

AgentResult SimpleAgent::runInternal(const std::string& input, TraceSession* trace_session) {
    if(input.empty()) {
        // 空字符串是调用边界错误，不进入 try 块，避免把明确输入问题包装成运行期执行失败。
        return AgentResult{false, "", "Agent 输入不能为空"};
    }

    try {
        // messages 是本次 run 的局部状态，任务结束后立即释放，
        // 避免首版 Agent 在未声明的情况下变成跨用户请求的会话或记忆容器。
        std::vector<Message> messages;
        if(!system_prompt_.empty()) {
            // 调用方可以显式置空系统提示词；Agent 不会自行添加第二条默认消息干扰该选择。
            messages.push_back(SystemMessage(system_prompt_));
        }
        messages.push_back(UserMessage(input));

        for(std::size_t request_index = 0; request_index < kMaxModelRequests; ++request_index) {
            // 每轮都重新读取低风险工具快照，使注册表后续扩展不会自动突破风险边界。
            // 当前同步执行模型下，调用方不得在 run 期间并发修改同一注册表。
            ChatRequest request;
            request.messages = messages;
            // 工具可见性在每次模型请求前从注册表快照得出，
            // 使未来注册的高风险工具默认不会泄露给 Agent 的模型上下文。
            request.tools = lowRiskTools(client_.tools());

            const ChatResponse response = trace_session == nullptr ? client_.chat(request) : client_.chat(request, *trace_session);
            // 即使 content 为空也必须保留 assistant 消息，因为它携带后续 ToolMessage 所关联的 Tool Call。
            messages.push_back(response.message);
            if(!response.hasToolCalls()) {
                // 不以文本是否为空判断结束；模型协议声明“无 Tool Call”才是 Agent 的自然终态。
                return AgentResult{true, response.content, ""};
            }

            const std::vector<ToolExecutionResult> execution_results = executeAllowedCalls(client_, response.tool_calls, trace_session);
            for(const ToolExecutionResult& execution : execution_results) {
                // 无论成功、未知工具还是受策略拒绝，结果都必须绑定原 tool_call_id 回填。
                // 模型据此决定修正参数、改用其他工具或直接给出最终说明。
                messages.push_back(execution.toToolMessage());
            }
            // 下一轮重新请求模型，让模型基于全部成功或失败观察自行选择继续方式。
        }
    } catch(const std::exception& exception) {
        // 所有标准异常收敛到 AgentResult，调用方可统一处理任务失败而不需要区分内部组件来源。
        // Provider、配置或运行期文件异常沿用 SDK 的异常语义，
        // Agent 只补充统一上下文并收敛到本次任务结果，不吞掉诊断原因。
        return AgentResult{false, "", "Agent 执行失败: " + std::string(exception.what())};
    }

    // 只有每次模型响应都继续请求工具时才会到达这里。
    // 固定熔断避免模型异常循环导致无限网络请求和不可控的工具副作用。
    return AgentResult{false, "", "Agent 达到内部安全熔断上限"};
}

}  // namespace aiSDK
