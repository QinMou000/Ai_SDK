#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>

#include "AIClient.h"

int main() {
    try {
        // 本示例只演示本地工具注册与执行，不发起模型请求。
        // 默认构造的 AIClient 会持有独立 ToolRegistry；
        // 即使没有 API Key，本地工具能力仍然可以离线使用和测试。
        aiSDK::AIClient client;

        // Tool 把“模型可见的协议描述”和本地处理函数分离。
        // 名称必须稳定，因为模型返回的 ToolCall::name 会用它查找处理函数；
        // parameters 使用标准 JSON Schema，required 阻止模型遗漏必要字段。
        const aiSDK::Tool add_tool{
            "add_numbers",
            "计算两个数字之和",
            nlohmann::json{
                           {"type", "object"},
                           {"properties",
                 {
                     {"left", {{"type", "number"}, {"description", "左操作数"}}},
                     {"right", {{"type", "number"}, {"description", "右操作数"}}},
                 }},
                           {"required", {"left", "right"}},
                           {"additionalProperties", false},
                           },
            aiSDK::ToolRiskLevel::Low,
        };

        // 处理函数只关心本地业务参数，不接触 Provider 或 HTTP 细节。
        // arguments.at/get 会对缺失字段和错误类型抛出异常；
        // ToolRegistry 会把这些异常转换为 ToolResult，而不是让批次中断。
        client.tools().registerTool(add_tool, [](const nlohmann::json& arguments) {
            const double left = arguments.at("left").get<double>();
            const double right = arguments.at("right").get<double>();
            return aiSDK::ToolResult::successResult({
                {"sum", left + right}
            });
        });

        // listTools 按首次注册顺序返回定义，可直接用于 ChatRequest::tools。
        // 这里打印列表，直观展示 SDK 门面已经持有工具定义。
        std::cout << "已注册工具:" << std::endl;
        for(const auto& tool : client.tools().listTools()) {
            std::cout << "- " << tool.name << ": " << tool.description << std::endl;
        }

        // 示例显式构造一批 Tool Call，模拟 Provider 已经解析出的模型响应。
        // AIClient::executeToolCalls 只执行该批本地调用，
        // 不会修改对话历史，也不会自动再次请求任何模型。
        const auto results = client.executeToolCalls({
            {"call_demo", "add_numbers", nlohmann::json{{"left", 19}, {"right", 23}}, R"({"left":19,"right":23})"},
        });
        // 返回值保留 ToolCall 和 ToolResult，调用方可以自行记录或转成 ToolMessage。
        // 生产调用方应先检查 success；本例参数固定合法，所以直接展示 data。
        // 结果使用 JSON 输出，便于与网络 Tool Call 示例保持一致的数据形态。
        // 到这里流程已经结束，没有任何隐藏的模型补充调用。
        std::cout << "执行结果: " << results.front().result.data.dump() << std::endl;
        return 0;
    } catch(const std::exception& exception) {
        // 示例入口统一返回非零退出码，方便脚本把配置或执行失败识别为失败。
        std::cerr << "工具注册示例执行失败: " << exception.what() << std::endl;
        return 1;
    }
}
