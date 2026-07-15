#include "tool/ToolExecutor.h"

namespace aiSDK {

Message ToolExecutionResult::toToolMessage() const {
    // Tool 角色消息的 content 是字符串，因此成功数据需要先序列化为 JSON 文本。
    // 失败时保留注册表生成的可诊断错误，不伪造成功结构。
    // call.id 原样写入 tool_call_id，保证下一次模型请求能关联原调用。
    // 本转换不访问注册表，也不会改变 ToolExecutionResult 自身。
    const std::string content = result.success ? result.data.dump() : result.error_message;
    return ToolMessage(content, call.id);
}

// 构造函数只保存注册表引用，工具定义和处理函数的所有权仍在调用方。
ToolExecutor::ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

std::vector<ToolExecutionResult> ToolExecutor::executeAll(const std::vector<ToolCall>& calls) {
    // 旧入口显式使用禁用会话，保证未选择 Trace 的调用不产生隐藏状态和开销。
    TraceSession disabled_trace;
    return executeAll(calls, disabled_trace);
}

std::vector<ToolExecutionResult> ToolExecutor::executeAll(const std::vector<ToolCall>& calls,
                                                          TraceSession& trace_session) {
    // 批次步骤承载汇总状态，单工具步骤只描述对应调用，职责保持分离。
    // Recorder 借用调用方会话，使模型请求和工具执行能够出现在同一时间线中。
    TraceRecorder recorder(trace_session);
    TraceStepScope batch_step = recorder.startStep(TraceStepType::ToolBatch);
    if(batch_step.enabled()) {
        // 空批次仍保留数量为零的成功步骤，便于区分未调用与无待执行工具。
        batch_step.setAttribute(TraceAttributeKey::ToolCount, calls.size());
    }

    // 结果数量始终与 calls 一致，调用方可按下标或 call.id 建立关联。
    // Trace 元数据不会写入返回对象，未启用观测的调用方无需承担协议变化。
    std::vector<ToolExecutionResult> results;
    // 预留完整容量，避免批量调用时因扩容移动已经生成的结果对象。
    results.reserve(calls.size());

    // 串行执行能够稳定保留模型调用顺序，也便于调用方关联结果消息。
    // registry_.execute 负责收敛单个调用的失败，因此这里无需提前终止批次。
    // 执行器不并发调度处理函数，避免改变既有注册表的线程安全要求。
    std::size_t success_count = 0;
    std::size_t failure_count = 0;
    try {
        for(std::size_t index = 0; index < calls.size(); ++index) {
            // 按输入下标串行推进，结果数组、Trace 序号和工具调用保持同一顺序。
            const ToolCall& call = calls[index];
            TraceStepScope tool_step;
            if(batch_step.enabled()) {
                // 父步骤创建失败时整体降级，避免子步骤误变成新的根步骤。
                // 该降级只影响观测链路，不跳过当前工具，也不改变返回结果数量。
                tool_step = recorder.startStep(TraceStepType::ToolExecution, batch_step.stepId());
            }
            if(tool_step.enabled()) {
                // 名称和输入位置属于诊断元数据；参数正文必须经过详情脱敏器。
                tool_step.setAttribute(TraceAttributeKey::ToolName, call.name);
                tool_step.setAttribute(TraceAttributeKey::ToolIndex, index);
            }
            if(tool_step.wantsDetails()) {
                // 详情策略由会话统一决定，执行器不自行推断哪些参数可安全保留。
                // raw_arguments 可能保留供应商原文，脱敏入口只接收已解析的 arguments。
                // 工具名通过上下文传入，允许调用方按工具制定不同的字段白名单。
                const TraceDetailContext context{TraceDetailKind::ToolArguments, call.name};
                tool_step.setSanitizedDetail(TraceDetailSlot::Arguments, context, call.arguments);
            }

            // 先把执行结果放入最终容器；若容器分配失败，scope 析构会记录未正常结束。
            // registry_.execute 会把预期的单工具失败收敛为 ToolResult，批次因此可继续。
            results.push_back(ToolExecutionResult{call, registry_.execute(call.name, call.arguments)});
            const ToolResult& result = results.back().result;
            if(tool_step.enabled()) {
                // 成功属性与步骤终态分别记录，导出方无需从详情正文推断执行结果。
                tool_step.setAttribute(TraceAttributeKey::Success, result.success);
            }
            if(tool_step.wantsDetails()) {
                // 失败文本只作为调用方脱敏器输入，默认不会进入 Trace。
                try {
                    // 统一信封让脱敏器可在成功数据和失败原因之间采取不同策略。
                    const nlohmann::json raw_result = result.success
                                                          ? nlohmann::json{{"success", true}, {"data", result.data}}
                                                          : nlohmann::json{{"success", false}, {"error_message", result.error_message}};
                    // 结果详情沿用当前工具上下文，不与相邻调用的脱敏规则混用。
                    const TraceDetailContext context{TraceDetailKind::ToolResult, call.name};
                    tool_step.setSanitizedDetail(TraceDetailSlot::Result, context, raw_result);
                } catch(...) {
                    // 详情准备失败不能把已完成的工具结果转换成批次异常。
                    // Trace 仍可依靠 success 属性和固定失败码完成结构化诊断。
                }
            }

            if(result.success) {
                // 每个工具都在进入下一项前收尾，保证快照不会长期暴露运行中状态。
                ++success_count;
                tool_step.succeed();
            } else {
                // 单项失败不抛出时继续后续调用，最终由批次状态表达部分失败。
                ++failure_count;
                tool_step.fail(TraceFailure::ToolExecutionFailed);
            }
        }
    } catch(...) {
        // 只有未被注册表收敛的异常才中断批次，并保留异常发生前的准确计数。
        // 批次失败记录使用固定码，避免异常正文或工具结果意外泄漏到 Trace。
        batch_step.setAttribute(TraceAttributeKey::SuccessCount, success_count);
        batch_step.setAttribute(TraceAttributeKey::FailureCount, failure_count);
        batch_step.fail(TraceFailure::ToolBatchException);
        throw;
    }
    // 正常遍历后统一写入汇总，空批次会自然得到两个零值计数。
    batch_step.setAttribute(TraceAttributeKey::SuccessCount, success_count);
    batch_step.setAttribute(TraceAttributeKey::FailureCount, failure_count);
    if(failure_count == 0U) {
        // 全部成功和空批次都属于完整完成，不需要额外制造失败语义。
        batch_step.succeed();
    } else {
        // 单工具失败仍返回完整结果，但批次 Trace 明确表达部分失败。
        // 汇总失败不会覆盖各子步骤的固定失败码，保留逐项定位能力。
        batch_step.fail(TraceFailure::ToolBatchPartialFailure);
    }
    // 返回完整批次；是否转换成消息或继续请求模型由上层显式决定。
    return results;
}

}  // namespace aiSDK
