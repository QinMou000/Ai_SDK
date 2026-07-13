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
    // 结果数量始终与 calls 一致，调用方可按下标或 call.id 建立关联。
    std::vector<ToolExecutionResult> results;
    // 预留完整容量，避免批量调用时因扩容移动已经生成的结果对象。
    results.reserve(calls.size());

    // 串行执行能够稳定保留模型调用顺序，也便于调用方关联结果消息。
    // registry_.execute 负责收敛单个调用的失败，因此这里无需提前终止批次。
    for(const auto& call : calls) {
        results.push_back(ToolExecutionResult{call, registry_.execute(call.name, call.arguments)});
    }
    // 返回完整批次；是否转换成消息或继续请求模型由上层显式决定。
    return results;
}

}  // namespace aiSDK
