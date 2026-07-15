#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mcp/MCPClient.h"
#include "mcp/MCPToolAdapter.h"

namespace {

// 本示例只演示“应用选择并执行一个远端工具”的最小闭环。
// Server 进程路径、工具名称和参数都来自命令行，因此入口先完成本地校验。
// 配置对象只描述连接，不会在构造 MCPClient 时启动进程或发送协议消息。
// connect 成功后 Client 才拥有逻辑会话，后续目录和调用都绑定该实例。
// 工具目录是带签发身份和代次的快照，不能由调用方自行拼装替代。
// Adapter 只转换经应用批准的选择，不会把 Server 全量工具隐式注册。
// 本地别名与远端名称分离，使模型侧名称可以保持稳定且便于审计。
// 示例把风险等级固定为高风险，提醒真实 Agent 在执行前增加用户批准。
// ToolRegistry 仍只负责一次本地 Handler 调用，不负责模型循环或消息历史。
// 工具业务失败通过 ToolResult 返回；连接与协议失败通过异常进入统一出口。
// 所有参数都作为独立 argv 字段传递，路径和参数不会经过 Shell 重新解释。
// close 在正常路径和异常路径都执行，保证后台线程及子进程按上限收敛。
// 退出码只表达示例成功、工具失败或输入错误，不承诺映射每个 MCP 错误码。
// 示例不缓存凭据、不写文件，也不会把 Server 返回的元数据当成可信指令。

// ClientCloser 强制示例在所有异常路径显式 close，而不是只依赖析构期尽力清理。
// MCPClient 的工具处理函数是同步入口，close 也会在配置上限内收敛后台任务。
class ClientCloser {
   public:
    explicit ClientCloser(std::shared_ptr<aiSDK::MCPClient> client) : client_(std::move(client)) {}

    ~ClientCloser() {
        if(client_) {
            client_->close();
        }
    }

    ClientCloser(const ClientCloser&) = delete;
    ClientCloser& operator=(const ClientCloser&) = delete;

   private:
    std::shared_ptr<aiSDK::MCPClient> client_;
};

aiSDK::MCPServerConfig makeServerConfig(int argc, char** argv) {
    // argv[1] 是唯一进程启动入口；先按 UTF-8 路径构造，再要求调用方给出绝对路径。
    // 绝对路径约束消除 PATH 查找差异，也避免工作目录变化后启动到不同程序。
    const std::filesystem::path executable = std::filesystem::u8path(argv[1]);
    if(!executable.is_absolute()) {
        throw std::invalid_argument("MCP Server 可执行文件必须使用绝对路径");
    }

    aiSDK::MCPStdioServerConfig stdio;
    // executable 与 arguments 分字段保存，Transport 据此调用平台无 Shell 进程接口。
    stdio.executable = executable;
    // 第四个参数之后只作为独立 argv 项传入 Server，绝不拼成 shell 命令。
    for(int index = 4; index < argc; ++index) {
        stdio.arguments.emplace_back(argv[index]);
    }
    stdio.working_directory = std::filesystem::current_path();
    // 示例显式选择继承，真实应用应按最小权限构造自己的环境白名单。
    stdio.inherit_parent_environment = true;

    aiSDK::MCPServerConfig config;
    // server_id 是非敏感逻辑标识，用于目录归属校验和上层诊断。
    // transport 变体明确选择 stdio；HTTP Server 应由应用构造另一种配置。
    config.server_id = "example_mcp";
    config.transport = std::move(stdio);
    return config;
}

void printCatalog(const aiSDK::MCPToolCatalog& catalog) {
    // 展示函数只读取快照，不注册、不调用，也不修改目录代次。
    // title 等附加元数据并非调用所需字段，因此这里只输出名称和描述。
    std::cout << "Server 返回的工具：\n";
    for(const auto& tool : catalog.tools()) {
        // Server 元数据只用于展示，不会自动降低远端工具的本地风险等级。
        std::cout << "  - " << tool.name;
        if(!tool.description.empty()) {
            std::cout << "：" << tool.description;
        }
        std::cout << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    // 参数不足在任何 JSON 解析或进程 I/O 之前失败，退出码 2 表示用法错误。
    if(argc < 4) {
        std::cerr << "用法：example_mcp_tool_call <Server绝对路径> <远端工具名> <JSON对象参数> [Server参数...]\n";
        return 2;
    }

    try {
        // argv[3] 必须独立解析为对象；标量和数组不能冒充 tools/call arguments。
        // JSON 语法异常沿标准异常路径报告，尚未创建 Client，因此没有资源需要回收。
        const nlohmann::json arguments = nlohmann::json::parse(argv[3]);
        if(!arguments.is_object()) {
            throw std::invalid_argument("工具参数必须是 JSON 对象");
        }

        // MCPClient 由 Agent / 应用层显式持有，不进入 AIClient，也不形成隐藏 Agent Loop。
        auto client = std::make_shared<aiSDK::MCPClient>(makeServerConfig(argc, argv));
        ClientCloser closer(client);
        client->connect();

        // 完整列举后由应用选择单个工具，并显式设置稳定别名与高风险等级。
        // 真实应用应在此处执行白名单、审批和用户授权，而不是信任 Server annotations。
        const aiSDK::MCPToolCatalog catalog = client->listTools();
        printCatalog(catalog);
        const std::vector<aiSDK::MCPToolSelection> selections = {
            {argv[2], std::string("selected_mcp_tool"), aiSDK::ToolRiskLevel::High},
        };
        const auto bindings = aiSDK::MCPToolAdapter::adaptTools(client, catalog, selections);

        // Adapter 没有隐式注册副作用；调用方审计 Binding 后才写入自己的注册表。
        // 注册表中的 Handler 捕获 Client 和签发目录，调用时仍会复核目录是否过期。
        // execute 返回后结果已经收敛成现有 ToolResult，便于上层复用既有工具通路。
        aiSDK::ToolRegistry registry;
        aiSDK::MCPToolAdapter::registerBindings(registry, bindings);
        const aiSDK::ToolResult result = registry.execute("selected_mcp_tool", arguments);
        if(result.success) {
            // 成功数据保持 JSON 结构，缩进输出仅服务于命令行可读性。
            std::cout << result.data.dump(2) << '\n';
        } else {
            // 失败文本由 Adapter 做有界净化，示例不据此判断是否重试。
            std::cerr << "MCP 工具执行失败：" << result.error_message << '\n';
        }

        // 目录刷新或连接关闭前由上层显式注销 Binding，避免旧定义继续进入模型请求。
        aiSDK::MCPToolAdapter::unregisterBindings(registry, bindings);
        client->close();
        return result.success ? 0 : 1;
    } catch(const std::exception& exception) {
        // 示例统一返回非零退出码；生产应用应优先按 MCPException 的结构化错误码分支。
        std::cerr << "MCP Tool Call 示例失败：" << exception.what() << '\n';
        return 1;
    }
}
