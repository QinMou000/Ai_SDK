#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "AIClient.h"
#include "agent/SimpleAgent.h"

namespace {

// 本示例展示推荐的最小组合方式：应用注册业务工具，SimpleAgent 只负责编排。
// 它不替换 AIClient 的 Provider、HTTP 或 ToolExecutor，因此可与已有示例并列作为上层用法参考。
// 默认提示词选择时间与目录列举，使真实模型有机会演示普通 Low 工具和工作区 Low 工具。
// 运行前必须由调用方理解当前工作目录即写入授权范围，示例不把仓库根目录硬编码为默认权限。
// 所有文件写操作仍由模型决定是否调用，应用应在生产环境使用最小的专用工作区。
// 流式文本会即时输出；模型不再请求工具时会自然结束，持续请求则由 Agent 内部安全熔断。
// Trace 通过显式会话传入，调用方可导出安全元数据而不需要 Agent 保存日志或会话状态。
// 错误路径只输出场景与异常原因，既不打印环境变量，也不读取或回显 .env 内容。
// get_current_time 与 add_numbers 都为纯本地计算，适合说明无需逐次审批的 Low 工具注册方式。
// 文件工具的风险等级来自受限根目录授权，不代表它们可以访问用户机器的任意路径。
// 命令行任务由模型解释，示例不实现文本命令解析或私有 Thought/Action 协议。
// 线上 Provider 的 API Key、地址和模型名沿用仓库既有 .env 约定，避免创建新的配置格式。
// 示例完成后进程退出，Agent 不保存此前消息，因此下一次启动不会带入任何任务历史。
// 该入口是在线示例；核心循环与文件边界由 tests/agent 中的脚本化 Provider 离线验证。
// 不传参数的默认任务不会修改文件，但真实模型仍可能根据自身工具选择做出不同的合理路径。
// 需要可预测无网络验证时，应运行 ai_sdk_agent_test，而不是依赖此示例的远端响应。
// 未来会话管理、记忆、审批或 MCP 能力应在此组合层扩展，不能侵入 AIClient 基础接口。
// 输出的 Trace JSON 适合开发诊断；生产应用可改为保存 snapshot 或接入自己的观测系统。
// 该文件保持 UTF-8 无 BOM，配合 /utf-8 编译选项确保中文任务和错误消息跨终端一致。
// 示例注册工具的名称与 README 说明同步，新增工具时应同时更新文档和离线风险测试。
// 工作区工具会拒绝 .env 和私钥，即使示例从包含配置文件的目录启动也不会把密钥交给模型。

// requireEnv 只读取在线示例必需的密钥，并在缺失时给出变量名而不回显内容。
// 示例配置仍完全通过 AIClient 进入既有 Provider 链路，不自行处理网络请求。
std::string requireEnv(const char* key) {
    // 不给空字符串兜底，避免示例把配置错误误表现为远端鉴权或网络问题。
    const std::string value = aiSDK::getEnvValue(key);
    if(value.empty()) {
        throw std::runtime_error(std::string("缺少环境变量: ") + key);
    }
    return value;
}

// getEnvOrDefault 让非敏感地址和模型参数保持与其他示例一致的可配置性。
std::string getEnvOrDefault(const char* key, const char* fallback) {
    // 仅对非敏感设置提供默认值，API Key 始终走 requireEnv 的显式失败路径。
    const std::string value = aiSDK::getEnvValue(key);
    return value.empty() ? fallback : value;
}

// joinPrompt 将命令行参数还原为一个任务文本。
// 未提供参数时给出会同时演示低风险工具和工作区工具的默认任务。
std::string joinPrompt(int argc, char** argv) {
    // 不做命令行转义或重写，示例只负责把已经分割的参数按显示顺序拼回任务文本。
    if(argc <= 1) {
        return "请先查询当前本地时间，再列出当前工作区的文件，并用中文简要说明结果。";
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

// currentLocalTime 是纯读取本机时间的低风险工具实现。
// 平台分支均使用线程安全时间转换函数，避免示例在并发调用时共享静态缓冲区。
std::string currentLocalTime() {
    // 读取失败通常表示系统时钟不可用，抛异常会被 ToolRegistry 转换为可观察的工具失败。
    const std::time_t now = std::time(nullptr);
    if(now == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("读取本地时间失败");
    }

    std::tm local_time{};
#ifdef _WIN32
    // Windows 的 localtime_s 与 POSIX 版本参数顺序不同，两个分支均不使用共享静态对象。
    if(localtime_s(&local_time, &now) != 0) {
        throw std::runtime_error("转换本地时间失败");
    }
#else
    // POSIX 分支使用 localtime_r 保证示例工具可被多个请求安全复用。
    if(localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("转换本地时间失败");
    }
#endif

    std::ostringstream formatted;
    // 固定数字格式避免把区域化月份或时区缩写作为模型后续解析的隐式输入。
    formatted << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

// requiredNumber 在工具边界验证模型参数，避免隐式字符串转换扩大工具语义。
double requiredNumber(const nlohmann::json& arguments, const char* key) {
    // 参数来自模型输出，必须在调用加法前完成类型检查，不能依赖 JSON 的隐式转换。
    if(!arguments.contains(key) || !arguments.at(key).is_number()) {
        throw std::invalid_argument(std::string("工具参数缺少数值字段: ") + key);
    }
    return arguments.at(key).get<double>();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // 示例从程序入口到结果输出只组合公开 SDK API，不接触 Provider JSON 或 HTTP 实现。
        // 从当前目录向上自动读取 .env，便于在仓库根目录和构建目录运行同一示例。
        aiSDK::loadNearestEnvFile(std::filesystem::current_path());

        aiSDK::Config config;
        // Provider 名称与现有示例一致，确保 .env 键和默认模型配置可直接复用。
        config.default_provider = "deepseek";
        // 示例显式启用 Trace，并把会话传给 Agent；默认 Trace 不保存消息和文件内容。
        config.enable_trace = true;
        config.providers["deepseek"] = aiSDK::ProviderConfig{
            requireEnv("DEEPSEEK_API_KEY"),
            getEnvOrDefault("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
            getEnvOrDefault("DEEPSEEK_MODEL", "deepseek-v4-flash"),
        };

        aiSDK::AIClient client(config);
        // 业务工具的注册顺序就是模型看到的顺序，两个定义都声明为可自动执行的 Low。
        // 这两个工具由示例应用注册，SimpleAgent 只会展示和执行 Low 风险工具。
        client.tools().registerTool(
            aiSDK::Tool{
                "get_current_time",
                "获取运行 SDK 的计算机当前本地时间",
                nlohmann::json{
                               {"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
                aiSDK::ToolRiskLevel::Low,
        },
            [](const nlohmann::json&) {
                // 返回对象而非裸字符串，便于模型稳定识别字段语义。
                return aiSDK::ToolResult::successResult({
                    {"local_time", currentLocalTime()}
                });
            });
        client.tools().registerTool(
            aiSDK::Tool{
                "add_numbers",
                "计算两个数的和，不会修改任何状态",
                nlohmann::json{{"type", "object"},
                               {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                               {"required", {"a", "b"}},
                               {"additionalProperties", false}},
                aiSDK::ToolRiskLevel::Low,
        },
            [](const nlohmann::json& arguments) {
                // 数值结果保持 double，避免示例因整数截断误导后续模型回答。
                const double left = requiredNumber(arguments, "a");
                const double right = requiredNumber(arguments, "b");
                return aiSDK::ToolResult::successResult({
                    {"sum", left + right}
                });
            });

        // 工作区根目录是显式授权边界。文件工具全部为 Low，
        // 但只能访问该目录以内的 UTF-8 文本，并会拒绝 .env、.git 和常见私钥路径。
        aiSDK::SimpleAgentOptions options;
        // current_path 由启动位置决定；生产调用应改为专用且最小化的业务工作目录。
        options.workspace_file_tools = aiSDK::WorkspaceFileToolOptions{std::filesystem::current_path()};
        aiSDK::SimpleAgent agent(client, std::move(options));

        aiSDK::TraceSession trace_session = client.startTrace();
        // 流式重载在当前线程即时回调文本；工具事件只显示状态，不默认输出参数或结果正文。
        // 显式 Trace 重载让所有流式模型请求和工具批次共享同一可导出会话。
        std::cout << "模型输出：" << std::flush;
        const aiSDK::AgentResult result = agent.runStream(
            joinPrompt(argc, argv),
            [](const aiSDK::AgentStreamEvent& event) {
                switch(event.type) {
                    case aiSDK::AgentStreamEventType::TextDelta:
                        std::cout << event.delta << std::flush;
                        return;
                    case aiSDK::AgentStreamEventType::ToolCallReady:
                        std::cout << "\n[工具就绪：" << event.tool_name << "]\n" << std::flush;
                        return;
                    case aiSDK::AgentStreamEventType::ToolExecutionFinished:
                        std::cout << "[工具" << (event.success ? "完成" : "失败") << "：" << event.tool_name << "]\n"
                                  << std::flush;
                        return;
                }
            },
            trace_session);
        std::cout << '\n';
        if(!result.success) {
            throw std::runtime_error(result.error_message);
        }

        // Trace 导出不包含默认的消息正文、文件内容或工具参数，因此可作为示例诊断输出。
        // 默认 Trace 只有调用次数、工具名和成功状态等安全元数据，可用于观察 ReAct 循环。
        std::cout << "\nTrace：\n" << trace_session.toJson().dump(2) << std::endl;
        return 0;
    } catch(const std::exception& exception) {
        // 统一为示例层补充上下文；底层原因保留在异常文本中但不主动记录配置密钥。
        std::cerr << "SimpleAgent 示例执行失败: " << exception.what() << std::endl;
        return 1;
    }
}
