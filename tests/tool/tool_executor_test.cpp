#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "tool/ToolExecutor.h"

namespace {

// makeTool 构造最小合法工具定义，测试重点放在批量执行与结果转换。
// 这里不声明业务字段，避免参数 Schema 校验干扰执行器顺序测试；
// 所有处理函数都通过捕获的 execution_order 记录真实执行时机。
aiSDK::Tool makeTool(const std::string& name) {
    return aiSDK::Tool{
        name,
        "记录调用顺序",
        nlohmann::json{
                       {"type", "object"},
                       {"properties", nlohmann::json::object()},
                       },
        aiSDK::ToolRiskLevel::Low,
    };
}

// 执行器承诺“每个输入都有一个结果”并严格保持输入顺序。
// 用例故意把未知工具放在两个合法调用中间，
// 同时验证失败结果位置和后续工具仍被实际调用。
TEST(ToolExecutorTest, ExecutesAllCallsInOrderAndContinuesAfterFailure) {
    // 注册表和顺序记录器都由测试持有，能够观察执行器是否偷偷复制状态；
    // 两个合法工具只写入名称，不依赖网络、时钟或随机数；
    // 因此本用例在 Windows 与 Linux 上都应得到完全一致的结果。
    aiSDK::ToolRegistry registry;
    std::vector<std::string> execution_order;

    registry.registerTool(makeTool("first"), [&](const nlohmann::json&) {
        execution_order.push_back("first");
        return aiSDK::ToolResult::successResult({
            {"position", 1}
        });
    });
    registry.registerTool(makeTool("last"), [&](const nlohmann::json&) {
        execution_order.push_back("last");
        return aiSDK::ToolResult::successResult({
            {"position", 3}
        });
    });

    // 中间放入未知工具，验证失败不会阻断最后一个工具。
    const std::vector<aiSDK::ToolCall> calls{
        {"call_1", "first",   nlohmann::json::object(), "{}"},
        {"call_2", "missing", nlohmann::json::object(), "{}"},
        {"call_3", "last",    nlohmann::json::object(), "{}"},
    };

    // ToolExecutor 只借用注册表，本次局部生命周期覆盖整个 executeAll 调用。
    // 输入 vector 在调用期间保持不变，执行器会把每个 ToolCall 复制进结果；
    // 因此返回后调用方仍能独立持有完整的诊断上下文。
    aiSDK::ToolExecutor executor(registry);
    const std::vector<aiSDK::ToolExecutionResult> results = executor.executeAll(calls);

    // 结果数量和顺序用于调用方关联模型的 call_id，属于核心契约。
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(results[0].result.success);
    // 未知工具被收敛为中间位置的失败结果，不会改变结果数组长度。
    EXPECT_FALSE(results[1].result.success);
    EXPECT_TRUE(results[2].result.success);
    // call.id 不能被执行器改写，否则 ToolMessage 无法关联模型请求。
    EXPECT_EQ(results[1].call.id, "call_2");
    // execution_order 不包含 missing，且 last 仍被执行，证明批次没有提前中止。
    EXPECT_EQ(execution_order, (std::vector<std::string>{"first", "last"}));
}

// ToolExecutionResult 需要转换成 OpenAI-compatible 的 Tool 角色消息。
// 成功结果应是可重新解析的 JSON 文本，失败结果应保留错误信息；
// 两种消息都必须携带原始 tool_call_id。
TEST(ToolExecutorTest, ConvertsExecutionResultToBoundToolMessage) {
    const aiSDK::ToolExecutionResult success{
        {"call_success", "lookup", nlohmann::json::object(), "{}"},
        aiSDK::ToolResult::successResult({{"answer", 42}}
        ),
    };

    // 消息必须绑定原 Tool Call ID，调用方才能安全拼接下一次请求。
    // 成功路径检查角色、调用标识和 JSON 数据三部分。
    const aiSDK::Message success_message = success.toToolMessage();
    // Tool 是供应商协议中的专用角色，不能退化成普通 assistant 文本。
    EXPECT_EQ(success_message.role, aiSDK::Role::Tool);
    // 可选字段在该转换后必须存在，先 ASSERT 再安全解引用。
    ASSERT_TRUE(success_message.tool_call_id.has_value());
    EXPECT_EQ(*success_message.tool_call_id, "call_success");
    EXPECT_EQ(nlohmann::json::parse(success_message.content).at("answer"), 42);

    // 失败路径不伪造 data，直接把可诊断文本交给上层或模型。
    // 使用独立 call_id，确保下面的断言不是偶然复用成功路径状态。
    const aiSDK::ToolExecutionResult failure{
        {"call_failure", "lookup", nlohmann::json::object(), "{}"},
        aiSDK::ToolResult::errorResult("查询失败"),
    };
    // 失败消息仍必须绑定原调用，否则后续请求无法满足供应商协议。
    const aiSDK::Message failure_message = failure.toToolMessage();
    // 失败文本保持原样，避免在消息转换层覆盖注册表提供的诊断上下文。
    EXPECT_EQ(failure_message.content, "查询失败");
    // 即使执行失败，供应商仍要求 tool_call_id 与原调用匹配。
    EXPECT_EQ(*failure_message.tool_call_id, "call_failure");
}

}  // namespace
