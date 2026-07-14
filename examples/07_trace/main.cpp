#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>

#include "AIClient.h"

// 运行结果用于展示业务返回，Trace JSON 用于展示独立可观测数据。
// 两者由调用方在示例入口显式组合，SDK 不隐式耦合输出策略。
// 生产应用可把相同 JSON 交给自己的存储或监控系统。
int main() {
    try {
        // Trace 由顶层配置显式开启；关闭时 startTrace 会返回禁用句柄。
        // 本示例完全离线，只演示本地工具链和结构化 JSON 导出。
        // 离线设计让示例在没有 API Key 时也能稳定运行和验证。
        // SDK 不会自动输出 Trace，是否展示或持久化始终由应用决定。
        aiSDK::Config config;
        config.enable_trace = true;
        aiSDK::AIClient client(config);

        // 工具定义和执行函数仍按现有 ToolRegistry 契约注册。
        // Trace 不改变参数、返回值、串行顺序，也不会自动发起模型请求。
        // 示例使用简单加法，避免业务逻辑掩盖步骤层级和导出结构。
        // 参数 Schema 仍会进入模型可见定义，但默认 Trace 不保存调用参数。
        const aiSDK::Tool add_tool{
            "trace_add",
            "计算两个整数之和",
            nlohmann::json{
                           {"type", "object"},
                           {"properties",
                 {
                     {"left", {{"type", "integer"}}},
                     {"right", {{"type", "integer"}}},
                 }},
                           {"required", {"left", "right"}},
                           },
            aiSDK::ToolRiskLevel::Low,
        };
        client.tools().registerTool(add_tool, [](const nlohmann::json& arguments) {
            const int left = arguments.at("left").get<int>();
            const int right = arguments.at("right").get<int>();
            return aiSDK::ToolResult::successResult({
                {"sum", left + right}
            });
        });

        // 同一个 TraceSession 可以继续传给 chat、streamChat 和后续工具批次。
        // 这里不配置详情脱敏器，因此参数与结果不会出现在 Trace JSON 中。
        // executeToolCalls 会创建一个批次根步骤和一个工具子步骤。
        // 两个步骤共享 trace_id，并按开始序号稳定导出。
        aiSDK::TraceSession trace_session = client.startTrace();
        const auto results = client.executeToolCalls(
            {
                {"call_trace_demo", "trace_add", nlohmann::json{{"left", 19}, {"right", 23}}, R"({"left":19,"right":23})"}
        },
            trace_session);

        // 业务结果仍由原接口返回；Trace 是独立的只读诊断数据。
        // 示例程序可以输出到标准流，SDK 库自身不会打印任何 Trace 内容。
        // snapshot() 适合程序内分析，toJson() 适合展示或交给外部存储。
        // 输出中应只看到工具名称、计数、状态与耗时等安全元数据。
        std::cout << "工具结果: " << results.front().result.data.dump() << std::endl;
        std::cout << "Trace JSON:" << std::endl << trace_session.toJson().dump(2) << std::endl;
        return 0;
    } catch(const std::exception& exception) {
        // 示例入口用非零退出码暴露配置或执行失败，便于本地脚本验证。
        // 旁路记录不会抛错；调用方显式执行的 JSON 导出仍可能报告分配失败。
        std::cerr << "Trace 示例执行失败: " << exception.what() << std::endl;
        return 1;
    }
}
