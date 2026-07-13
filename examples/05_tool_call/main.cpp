#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "AIClient.h"

namespace {

// requireEnv 读取在线示例的必需配置。
// API Key 缺失时立即失败，避免构造出必然被远端拒绝的请求；
// 错误只包含变量名，不会把密钥内容写入日志。
std::string requireEnv(const char* key) {
    const std::string value = aiSDK::getEnvValue(key);
    if(value.empty()) {
        throw std::runtime_error(std::string("缺少环境变量: ") + key);
    }
    return value;
}

// getEnvOrDefault 用于非敏感的地址和模型配置。
// 环境变量有值时尊重用户选择，否则回退到示例的稳定默认值；
// 该函数不负责加载 .env，加载动作统一放在 main 的入口阶段。
std::string getEnvOrDefault(const char* key, const char* fallback) {
    const std::string value = aiSDK::getEnvValue(key);
    return value.empty() ? fallback : value;
}

// joinPrompt 把命令行剩余参数恢复成一个完整用户问题。
// 这样既支持不带参数直接运行，也支持包含空格的临时提示词；
// 参数之间只补一个空格，不改写调用方提供的其他字符。
std::string joinPrompt(int argc, char** argv) {
    if(argc <= 1) {
        return "请调用工具查询当前本地时间，然后用一句中文告诉我。";
    }

    std::ostringstream prompt;
    for(int index = 1; index < argc; ++index) {
        if(index > 1) {
            prompt << ' ';
        }
        prompt << argv[index];
    }
    return prompt.str();
}

// currentLocalTime 是本示例注册的真实本地工具实现。
// 它只读取系统时间，不访问网络、不修改状态，属于低风险工具；
// 返回统一格式的本地时间字符串，便于后续包装成结构化 JSON。
std::string currentLocalTime() {
    const std::time_t now = std::time(nullptr);
    if(now == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("读取本地时间失败");
    }

    std::tm local_time{};
#ifdef _WIN32
    // MSVC 提供参数顺序为 destination/source 的线程安全 localtime_s。
    if(localtime_s(&local_time, &now) != 0) {
        throw std::runtime_error("转换本地时间失败");
    }
#else
    // Linux 使用 localtime_r，避免 std::localtime 返回共享静态缓冲区。
    if(localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("转换本地时间失败");
    }
#endif

    // 输出固定格式，避免区域化名称和时区缩写给模型带来歧义。
    // 时区仍遵循运行 SDK 的操作系统设置，工具描述会明确这一点。
    std::ostringstream formatted;
    formatted << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // 阶段一：加载配置。
        // 与其他在线示例保持一致，从当前目录向上查找最近的 .env；
        // 因此从仓库根目录或 build 子目录启动都能使用同一份本地配置。
        aiSDK::loadNearestEnvFile(std::filesystem::current_path());

        // Provider 配置仍通过 AIClient 进入统一聊天链路。
        // Tool Call 能力不会绕过现有 DeepSeekProvider 或自行拼接 HTTP 请求。
        aiSDK::Config config;
        config.default_provider = "deepseek";
        config.providers["deepseek"] = aiSDK::ProviderConfig{
            requireEnv("DEEPSEEK_API_KEY"),
            getEnvOrDefault("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
            getEnvOrDefault("DEEPSEEK_MODEL", "deepseek-v4-flash"),
        };

        aiSDK::AIClient client(config);
        // 阶段二：注册模型可见的工具定义和本地处理函数。
        // Schema 明确声明不接收参数，防止模型附带未支持字段；
        // 处理函数返回对象形式的 ToolResult，方便模型理解字段语义。
        client.tools().registerTool(
            aiSDK::Tool{
                "get_current_time",
                "获取运行 SDK 的计算机当前本地时间",
                nlohmann::json{
                               {"type", "object"},
                               {"properties", nlohmann::json::object()},
                               {"additionalProperties", false},
                               },
                aiSDK::ToolRiskLevel::Low,
        },
            [](const nlohmann::json&) {
                return aiSDK::ToolResult::successResult({
                    {"local_time", currentLocalTime()}
                });
            });

        // 阶段三：由调用方构造一次普通 ChatRequest。
        // 工具列表通过公开注册表显式注入，AIClient::chat 的既有语义保持不变；
        // SDK 不会因为注册过工具就偷偷修改任意请求。
        aiSDK::ChatRequest request;
        request.messages.push_back(aiSDK::SystemMessage("你是一个简洁的中文助手；需要当前时间时必须调用提供的工具。"));
        request.messages.push_back(aiSDK::UserMessage(joinPrompt(argc, argv)));
        request.tools = client.tools().listTools();

        // 第一次请求只负责让模型选择直接回答或返回 Tool Call。
        // DeepSeekProvider 会把供应商 JSON 解析成统一 ChatResponse。
        const aiSDK::ChatResponse first_response = client.chat(request);
        if(!first_response.hasToolCalls()) {
            // 模型可以选择直接回答；SDK 不会强制产生 Tool Call。
            // 此分支直接输出模型内容，避免把空工具批次当成错误。
            std::cout << first_response.content << std::endl;
            return 0;
        }

        // 阶段四：调用方显式要求 SDK 执行本次响应中的工具调用。
        // 返回结果与模型调用顺序一一对应，未知工具或本地异常也会保留为失败结果。
        const auto execution_results = client.executeToolCalls(first_response.tool_calls);
        // OpenAI-compatible 协议要求先保留携带 tool_calls 的 assistant 消息，
        // 再追加每个绑定 tool_call_id 的 Tool 角色结果消息。
        request.messages.push_back(first_response.message);
        for(const auto& execution : execution_results) {
            // 每条消息由结果对象保留原 call.id，多个 Tool Call 也能正确配对。
            request.messages.push_back(execution.toToolMessage());
        }

        // 阶段五：示例应用自行决定把结果交回模型以生成自然语言答案。
        // 这只是调用方显式发起的第二次 chat，不是 AIClient 内部的 Agent Loop；
        // SDK 不判断是否继续、不重试，也不管理后续会话生命周期。
        // 示例只补充一次请求：即使模型再次返回 Tool Call，也不会在这里自动循环。
        // 这种写法刻意展示 SDK 的组合能力，而不是提供一个隐藏的 Agent 实现。
        const aiSDK::ChatResponse final_response = client.chat(request);
        // 最终文本仍走普通 ChatResponse 接口，与不使用工具时保持一致。
        // 调用方若需要 Trace 或持久化，可自行保存 request 和 execution_results。
        std::cout << final_response.content << std::endl;
        return 0;
    } catch(const std::exception& exception) {
        // 在线示例统一输出带场景名称的错误，便于区分配置、网络和工具失败。
        // API Key 本身不会出现在这里，避免示例日志泄露敏感配置。
        std::cerr << "Tool Call 示例执行失败: " << exception.what() << std::endl;
        return 1;
    }
}
