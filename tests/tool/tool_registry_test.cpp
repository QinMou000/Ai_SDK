#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tool/ToolRegistry.h"

namespace {

// makeTool 让测试只关注注册表行为，避免在每个用例重复构造 Schema。
// 返回的 parameters 是合法对象 Schema，满足 registerTool 的公共前置条件；
// 每个测试只替换与目标场景相关的名称、描述或 Schema。
// 独立辅助函数也让后续扩展 Schema 时只需维护一份测试夹具。
aiSDK::Tool makeTool(std::string name, std::string description = "测试工具") {
    return aiSDK::Tool{
        std::move(name),
        std::move(description),
        nlohmann::json{
                       {"type", "object"},
                       {"properties", nlohmann::json::object()},
                       },
        aiSDK::ToolRiskLevel::Low,
    };
}

// 验证最常用的注册—列举—执行通路。
// 这个用例同时锁定稳定顺序，因为 unordered_map 本身不保证迭代次序；
// 如果顺序漂移，发送给模型的工具列表和调试输出也会随机变化。
TEST(ToolRegistryTest, RegistersListsAndExecutesToolsInStableOrder) {
    aiSDK::ToolRegistry registry;

    // 注册顺序属于公开可观察行为，模型请求和示例输出都应保持稳定。
    registry.registerTool(makeTool("first"), [](const nlohmann::json&) {
        return aiSDK::ToolResult::successResult({
            {"value", 1}
        });
    });
    registry.registerTool(makeTool("second"), [](const nlohmann::json&) {
        return aiSDK::ToolResult::successResult({
            {"value", 2}
        });
    });

    // 先从只读查询面验证元数据和注册顺序。
    const std::vector<aiSDK::Tool> tools = registry.listTools();
    ASSERT_EQ(tools.size(), 2U);
    EXPECT_EQ(tools[0].name, "first");
    EXPECT_EQ(tools[1].name, "second");
    // hasTool 适合调用方在执行前进行非抛异常探测。
    EXPECT_TRUE(registry.hasTool("first"));
    // getTool 返回最新定义副本，不暴露内部容器的可变引用。
    EXPECT_EQ(registry.getTool("second").description, "测试工具");

    // 再从执行面验证参数能够进入处理函数，结果保持结构化 JSON。
    const aiSDK::ToolResult result = registry.execute("first", nlohmann::json::object());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.data.at("value"), 1);
}

// 同名工具更新是刻意支持的 SDK 行为。
// 更新必须同时替换模型定义和本地处理函数，
// 但不能把旧名称再次追加到顺序表造成重复工具。
TEST(ToolRegistryTest, ReplacesDuplicateToolWithoutChangingOrder) {
    aiSDK::ToolRegistry registry;

    registry.registerTool(makeTool("stable", "旧定义"), [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(1); });
    registry.registerTool(makeTool("other"), [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(2); });

    // 同名注册用于更新实现，但工具在请求中的相对顺序不能因此抖动。
    registry.registerTool(makeTool("stable", "新定义"), [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(3); });

    // 列表中只能保留一份 stable，并继续位于首次注册的位置。
    const std::vector<aiSDK::Tool> tools = registry.listTools();
    ASSERT_EQ(tools.size(), 2U);
    EXPECT_EQ(tools[0].name, "stable");
    EXPECT_EQ(tools[0].description, "新定义");
    // other 仍排在第二位，证明 stable 的替换没有追加新顺序项。
    EXPECT_EQ(tools[1].name, "other");
    // 执行结果为 3，证明处理函数也与定义一起完成了替换。
    EXPECT_EQ(registry.execute("stable", nlohmann::json::object()).data, 3);
}

// 注册阶段负责拒绝无法进入模型协议或无法执行的半成品工具。
// 这些错误属于调用方定义问题，因此约定抛 std::invalid_argument；
// 执行阶段的未知工具则采用 ToolResult，两类边界不能混淆。
TEST(ToolRegistryTest, RejectsInvalidDefinitionsAtRegistrationBoundary) {
    aiSDK::ToolRegistry registry;

    // 空名称、空处理函数和非对象 Schema 都无法形成可执行的模型工具。
    EXPECT_THROW(registry.registerTool(makeTool(""), [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(nullptr); }), std::invalid_argument);
    EXPECT_THROW(registry.registerTool(makeTool("missing_handler"), aiSDK::ToolHandler{}), std::invalid_argument);

    // 顶层数组不是函数工具要求的 parameters 对象。
    aiSDK::Tool invalid_schema = makeTool("invalid_schema");
    invalid_schema.parameters = nlohmann::json::array();
    EXPECT_THROW(registry.registerTool(invalid_schema, [](const nlohmann::json&) { return aiSDK::ToolResult::successResult(nullptr); }), std::invalid_argument);
}

// 工具名称来自模型输出，未知名称属于可恢复的运行期结果。
// 本地处理函数也是扩展代码，可能抛标准异常；
// 两种失败都应被收敛，以便同一批的其他工具继续执行。
TEST(ToolRegistryTest, ConvertsUnknownToolsAndHandlerExceptionsToResults) {
    aiSDK::ToolRegistry registry;

    // 未知工具是模型输出边界问题，应成为失败结果而不是异常。
    const aiSDK::ToolResult missing = registry.execute("missing", nlohmann::json::object());
    // 失败结果使用 success=false，调用方无需依赖错误文本做流程判断。
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.error_message.find("工具不存在"), std::string::npos);

    // 使用明确异常文本，验证注册表既补充工具名，也保留底层原因。
    registry.registerTool(makeTool("throws"), [](const nlohmann::json&) -> aiSDK::ToolResult { throw std::runtime_error("测试异常"); });

    // 工具实现属于扩展代码，其异常必须在注册表边界收敛。
    const aiSDK::ToolResult failed = registry.execute("throws", nlohmann::json::object());
    // 处理函数异常同样使用稳定失败结构，避免异常穿透 SDK 工具边界。
    EXPECT_FALSE(failed.success);
    EXPECT_NE(failed.error_message.find("工具执行失败: throws"), std::string::npos);
    EXPECT_NE(failed.error_message.find("测试异常"), std::string::npos);
}

}  // namespace
