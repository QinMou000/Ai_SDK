# AI SDK for C++

## 项目简介
这是一个面向 ChatBot、Agent 和 AI 应用的纯 C++ AI SDK。当前仓库已经具备可本地构建、可运行真实 DeepSeek API 调用、可执行基础测试的最小闭环。

当前已经落地的重点包括：

* 根目录统一使用 `CMake + vcpkg + CMake Presets`
* `include/`、`src/`、`examples/`、`tests/` 的模块化骨架
* `DeepSeekProvider` 的真实 `chat` / `streamChat` 请求链路
* 基于 `.env`、环境变量和 JSON 配置占位符的本地配置读取
* 本地 C++ 工具注册、稳定列举、串行执行与异常收敛
* 由 `AIClient::executeToolCalls(...)` 暴露的显式单批 Tool Call 执行接口
* 显式 `TraceSession`、线程安全步骤快照、默认安全元数据与 JSON 导出
* 独立可选的 MCP Client，支持本地 stdio 与 MCP Streamable HTTP，并可显式适配到现有 `ToolRegistry`

## 目录结构

```text
ai_sdk/
  CMakeLists.txt
  CMakePresets.json
  CMakeUserPresets.json
  vcpkg.json
  README.md

  docs/
    PRD.md
    跨平台构建方案.md

  include/
    AIClient.h
    core/
    provider/
    http/
    tool/
    trace/
    mcp/
    agent/

  src/
    AIClient.cpp
    core/
    provider/
    http/
    tool/
    trace/
    mcp/
    agent/

  examples/
    01_chat_deepseek/
    02_chat_minimax/
    03_stream_chat/
    04_register_tool/
    05_tool_call/
    06_simple_agent/
    07_trace/
    08_mcp_tool_call/

  tests/
    smoke/
    core/
    provider/
    tool/
    http/
    trace/
    mcp/
```

## 依赖要求

### 通用要求

* CMake 3.23 及以上
* 支持 C++17 的编译器
* Ninja
* Git
* vcpkg

### 当前 `vcpkg.json` 依赖

* `cpr`
* `nlohmann-json`
* `spdlog`
* `gtest`

## 构建方式

### 推荐方式

项目默认通过 `CMakePresets.json` 管理构建配置。

公共预设：

* `windows-debug`
* `windows-release`
* `linux-debug`
* `linux-release`

本地预设：

* `local-windows-debug`
* `local-windows-bootstrap`
* `local-windows-release`

其中：

* `CMakePresets.json` 用于仓库共享配置
* `CMakeUserPresets.json` 用于本机私有配置
* 当前本机私有配置里，`VCPKG_ROOT` 指向 `D:/vcpkg`

`AISDK_BUILD_MCP` 默认开启，并生成独立的 `ai_sdk_mcp` 目标。应用使用 MCP 时需要显式链接该目标；核心 `ai_sdk` 不会反向依赖或持有 MCP 连接。若当前构建完全不需要 MCP，可在配置时传入 `-DAISDK_BUILD_MCP=OFF`，此时 MCP 源文件、示例和测试目标都不会进入构建图。

### Windows 本地编译

如果终端没有自动加载 Visual Studio 开发者环境，建议先进入 `vcvars64.bat`。

方式 A：先加载开发者环境，再使用本地预设

```cmd
REM 加载 64 位 MSVC 编译工具链，使当前终端可以使用 cl.exe 等构建工具。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat

REM 使用本机的 Debug 预设检查依赖并生成 Ninja 构建文件。
cmake --preset local-windows-debug

REM 按 Debug 预设编译核心库、示例和测试，并输出完整构建命令。
cmake --build --preset local-windows-debug -v

REM 运行 Debug 预设对应的全部测试，并在失败时显示详细输出。
ctest --preset local-windows-debug --output-on-failure
```

方式 B：一次性执行

```cmd
REM 加载 64 位 MSVC 工具链，然后配置 Debug 构建目录。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && cmake --preset local-windows-debug

REM 加载 64 位 MSVC 工具链，然后编译 Debug 目标并输出完整构建命令。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && cmake --build --preset local-windows-debug -v

REM 加载 64 位 MSVC 工具链，然后运行全部测试并显示失败详情。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && ctest --preset local-windows-debug --output-on-failure
```

### Windows Release 构建

```cmd
REM 加载 64 位 MSVC 编译工具链。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat

REM 使用本机的 Release 预设检查依赖并生成优化构建文件。
cmake --preset local-windows-release

REM 按 Release 预设编译核心库和示例，并输出完整构建命令。
cmake --build --preset local-windows-release -v
```

### Windows 快速验证

如果只想验证核心库和依赖是否能正确配置，可以使用 bootstrap 预设：

```cmd
REM 加载 64 位 MSVC 编译工具链。
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat

REM 配置仅包含核心库的 bootstrap 构建目录，不启用示例和测试。
cmake --preset local-windows-bootstrap

REM 编译 bootstrap 目标，快速验证工具链、依赖和核心库入口。
cmake --build --preset local-windows-bootstrap -v
```

这个预设会关闭 `examples` 和 `tests`，只验证核心库入口，适合排查工具链问题。

### Linux 本地编译

Linux 使用仓库共享的 `linux-debug` 和 `linux-release` 预设，不使用 Windows 专属的 `local-windows-*` 预设。开始构建前，请确保已经安装支持 C++17 的 GCC 或 Clang、CMake 3.23 及以上版本、Ninja、Git 和 vcpkg。

以下示例假设 vcpkg 安装在 `$HOME/vcpkg`。如果实际安装位置不同，请替换为对应的绝对路径。

#### Linux Debug 构建与测试

```bash
# 设置 vcpkg 根目录，供 CMake 预设定位 vcpkg 工具链文件。
export VCPKG_ROOT="$HOME/vcpkg"

# 使用共享的 Linux Debug 预设检查依赖并生成 Ninja 构建文件。
cmake --preset linux-debug

# 按 Linux Debug 预设编译核心库、示例和测试，并输出完整构建命令。
cmake --build --preset linux-debug -v

# 运行 Linux Debug 预设对应的全部测试，并在失败时显示详细输出。
ctest --preset linux-debug --output-on-failure
```

#### Linux Release 构建

在同一个已经设置 `VCPKG_ROOT` 的终端中执行：

```bash
# 使用共享的 Linux Release 预设检查依赖并生成优化构建文件。
cmake --preset linux-release

# 按 Linux Release 预设编译核心库和示例，并输出完整构建命令。
cmake --build --preset linux-release -v
```

当前仓库没有 Linux bootstrap 预设；如果只需要快速验证 Linux 核心库，应先在 `CMakePresets.json` 中补充对应预设，而不是复用 Windows 专属预设。

## `.env` 配置方式

### 支持范围

当前仓库已经支持从 `.env` 文件读取密钥和默认配置，主要覆盖三类入口：

* `examples/01_chat_deepseek`
* `examples/03_stream_chat`
* `examples/05_tool_call`
* `loadConfigFromFile(...)`

这几个入口都会从当前目录向上查找最近的 `.env` 文件。也就是说：

* 你可以把 `.env` 放在仓库根目录
* 即使从 `build/windows-debug/...` 子目录启动示例，也能自动找到它

### 推荐 `.env` 内容

```dotenv
DEEPSEEK_API_KEY=你的_api_key
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-v4-flash
```

### JSON 配置文件中的占位符

如果你使用 `loadConfigFromFile` 加载 JSON 配置，还可以在 JSON 里继续写占位符：

```json
{
  "providers": {
    "deepseek": {
      "api_key": "${DEEPSEEK_API_KEY}",
      "base_url": "${DEEPSEEK_BASE_URL}",
      "default_model": "${DEEPSEEK_MODEL}"
    }
  },
  "default_provider": "deepseek",
  "timeout_ms": 30000,
  "enable_trace": true
}
```

## 示例运行

### 普通聊天示例

构建完成后运行：

```powershell
.\build\windows-debug\examples\01_chat_deepseek\example_chat_deepseek.exe "请介绍一下你自己"
```

如果不传命令行参数，示例会使用默认提示词。

### 流式聊天示例

```powershell
.\build\windows-debug\examples\03_stream_chat\example_stream_chat.exe "请流式介绍一下你自己"
```

该示例会实时输出模型返回的增量文本。

### 离线工具注册示例

该示例不需要 API Key，用于验证工具定义、注册表和本地执行入口：

```powershell
.\build\windows-debug\examples\04_register_tool\example_register_tool.exe
```

### Tool Call 示例

配置 `DEEPSEEK_API_KEY` 后，可以运行真实 DeepSeek Tool Call 示例：

```powershell
.\build\windows-debug\examples\05_tool_call\example_tool_call.exe "请查询当前本地时间"
```

SDK 只负责模型协议适配和调用方显式指定的本地工具执行，不会自动形成 Agent Loop。基本调用关系如下：

```cpp
AIClient client(config);

client.tools().registerTool(tool, handler);

ChatRequest request;
request.messages.push_back(UserMessage("请查询当前本地时间"));
request.tools = client.tools().listTools();

ChatResponse response = client.chat(request);
std::vector<ToolExecutionResult> results =
    client.executeToolCalls(response.tool_calls);

// 是否追加 assistant/tool 消息并再次调用 chat，由上层应用决定。
```

`executeToolCalls(...)` 保持模型返回顺序。未知工具或本地处理函数异常会转换为失败的 `ToolResult`，不会阻断同一批中的其他工具调用。

### Trace 链路追踪

Trace 默认关闭。开启 `Config::enable_trace` 后，调用方通过 `startTrace()` 创建显式会话，并把同一个会话传给需要关联的模型请求、工具执行和后续模型请求：

```cpp
Config config;
config.enable_trace = true;
AIClient client(config);

TraceSession trace = client.startTrace();

ChatResponse first = client.chat(request, trace);
std::vector<ToolExecutionResult> results =
    client.executeToolCalls(first.tool_calls, trace);

// 是否补充消息并再次请求仍由上层应用显式决定。
ChatResponse final = client.chat(request, trace);

Trace snapshot = trace.snapshot();
nlohmann::json trace_json = trace.toJson();
```

每个公开操作都是同一 `trace_id` 下的新根步骤；Provider、HTTP、SSE 和单个工具步骤通过明确的 `parent_step_id` 形成内部层级。SDK 不会根据“上一次操作”推断父节点，也不会隐式形成 Agent Loop。

默认 Trace 只保存 Provider、Model、步骤类型、状态、HTTP 状态码、耗时、SSE 与工具增量数量、工具名称及成功计数等元数据。它不会自动保存以下内容：

* API Key 或完整 `Authorization` 头
* URL、用户消息、完整请求体或响应正文
* SSE 文本增量、工具参数、工具结果或底层异常原文

确需业务详情时，可以提供返回 JSON 对象的脱敏器。`TraceDetailContext::kind` 表示原始输入类别，`operation_name` 表示当前操作名称：模型请求与响应使用 Provider 名称，工具参数与结果使用工具名称。只有脱敏器返回的顶层对象会写入 `details`：

```cpp
TraceOptions options;
options.detail_sanitizer = [](const TraceDetailContext& context, const nlohmann::json& raw) {
    if(context.kind == TraceDetailKind::ToolArguments &&
       context.operation_name == "get_current_time") {
        return nlohmann::json{{"field_count", raw.size()}};
    }
    return nlohmann::json{{"recorded", true}};
};

TraceSession trace = client.startTrace(options);
```

每个已处理的详情槽位都固定为 `{"status": ..., "value": {...}}`。脱敏器正常返回对象时状态为 `recorded`；返回数组、标量或 `null` 时状态为 `rejected`；抛出异常时状态为 `sanitizer_failed`。后两种状态的 `value` 都是空对象，且不会保存异常文本或原始值。例如：

```json
{
  "details": {
    "arguments": {
      "status": "recorded",
      "value": {
        "field_count": 2
      }
    }
  }
}
```

脱敏器会接收四类原始输入，调用方必须按类别建立自己的字段白名单：

* `ModelRequest`：`chatRequestToJson(request)` 的完整对象，可能包含消息、工具定义和模型参数。
* `ModelResponse`：`chatResponseToJson(response)` 的完整对象，可能包含正文、工具调用、用量和 `raw_response`。
* `ToolArguments`：解析后的 `ToolCall::arguments` 对象，不传入 `raw_arguments`。
* `ToolResult`：成功时为 `{"success": true, "data": ...}`，失败时为 `{"success": false, "error_message": ...}`。

Trace JSON 的每个步骤都固定包含 `error_code` 和 `error_summary`。成功步骤的 `error_code` 为 `none`；失败步骤使用 SDK 固定枚举映射，禁止写入任意错误码或底层异常原文。

`TraceSession` 可复制，副本共享同一线程安全状态；并发追加、读取快照和 JSON 导出不会丢步骤，输出按步骤开始序号排序。自定义脱敏器自身的并发安全由调用方保证。`ToolRegistry` 仍不是线程安全容器，不能因为 Trace 会话线程安全而省略工具注册表的外部互斥。

离线示例不需要 API Key：

```powershell
.\build\windows-debug\examples\07_trace\example_trace.exe
```

迁移说明：早期预留的 `TraceRecorder(Trace)`、`addStep(...)` 和返回内部引用的 `snapshot()` 已移除。新代码应从 `AIClient::startTrace()` 获取会话，通过带 `TraceSession&` 的公开重载记录链路，并使用 `TraceSession::snapshot()` 按值读取稳定快照。

### MCP Tool Call 示例

MCP 由 Agent 或应用层显式持有，不并入 `AIClient`，也不会自动形成 Agent Loop。一个 `MCPClient` 对应一个 Server 和一个逻辑会话；每个实例最多允许一个用户公开操作在途，后台 GET SSE、通知处理、状态读取和 `close()` 不占该名额。需要并行工具调用时，应由上层创建多个 Client 或 Client Pool。

下面是 stdio Client 的最小配置。可执行文件必须是绝对路径，参数按独立 `argv` 项传递，SDK 不经过 shell；Windows 的 `.cmd`、`.bat` 或其他脚本必须由应用显式指定真实解释器：

```cpp
aiSDK::MCPStdioServerConfig stdio;
stdio.executable = std::filesystem::path("D:/tools/mcp-server.exe");
stdio.arguments = {"--mode", "stdio"};
stdio.working_directory = std::filesystem::current_path();
stdio.inherit_parent_environment = false;

aiSDK::MCPServerConfig config;
config.server_id = "local_tools";
config.transport = std::move(stdio);

auto client = std::make_shared<aiSDK::MCPClient>(std::move(config));
client->connect();
const aiSDK::MCPToolCatalog catalog = client->listTools();
// 由应用执行白名单、审批和别名映射后，再显式创建并注册 Binding。
client->close();
```

Streamable HTTP 使用 `MCPStreamableHttpConfig`，生产 Endpoint 只接受 HTTPS。开发期明文 HTTP 必须显式开启 `allow_loopback_http`，且地址只能是字面量 IPv4/IPv6 回环地址。实现始终校验证书和主机名、禁止自动重定向并显式忽略环境代理；首版不暴露自定义 CA、mTLS、代理、代理认证或关闭 TLS 校验的生产入口。初始化后会默认尝试建立后台 GET SSE；Server 返回 405 时正常降级为仅处理 POST JSON/SSE。

远端工具不会被自动注册。应用必须从签发的 Catalog 中选择工具，设置本地别名和风险级别，再通过 `MCPToolAdapter` 生成 Binding。所有远端工具默认高风险；目录发生 `tools/list_changed` 后，旧 Catalog 与 Binding 会失效，上层需要先批量注销，再重新列举、审核和绑定。Adapter 的同步 Handler 会阻塞调用线程直到结果或超时，并且首版只适配文本与对象形态的 `structuredContent`；富媒体结果仍可从 `MCPClient::callTool()` 的无损结果读取，但不能进入现有 `ToolResult`。

构建后可运行离线 stdio 示例：

```powershell
.\build\windows-debug\examples\08_mcp_tool_call\example_mcp_tool_call.exe `
  "D:\tools\mcp-server.exe" "远端工具名" "{}" "--server-arg"
```

示例和真实应用都应显式调用 `close()`；析构函数只提供有界的最佳努力清理。若配置 stderr 回调，回调内容来自不受信任的 Server，应用必须自行脱敏。

## 测试方式

### 全量测试

```powershell
ctest --preset local-windows-debug --output-on-failure
```

### Provider 在线集成测试

`tests/provider/ai_sdk_provider_test` 会优先加载最近的 `.env`。如果没有发现 `DEEPSEEK_API_KEY`，测试会自动跳过，而不是失败。

单独执行方式：

```powershell
.\build\windows-debug\tests\provider\ai_sdk_provider_test.exe
```

### 当前测试覆盖

* `tests/smoke/ai_sdk_smoke_test.cpp`
* `tests/core/ai_sdk_core_test`
* `tests/provider/ai_sdk_provider_test`
* `tests/tool/ai_sdk_tool_test`
* `tests/http/ai_sdk_http_test`
* `tests/trace/ai_sdk_trace_test`
* `tests/mcp/ai_sdk_mcp_test`
* `tests/mcp/ai_sdk_mcp_stdio_test`
* `tests/mcp/ai_sdk_mcp_http_test`

MCP 开启时，顶层 CTest 清单固定包含 `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test` 和 `ai_sdk_mcp_http_test` 三项，并统一带有 `mcp` 标签。可单独执行：

```powershell
ctest --preset windows-debug -L mcp --output-on-failure --no-tests=error
```

## 本地环境说明

当前这台机器上的已知本地约定如下：

* `vcpkg` 路径：`D:\vcpkg`
* Visual Studio 开发者环境入口：`D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat`
* 本地调试预设：`local-windows-debug`

如果更换机器，只需要同步修改自己的 `CMakeUserPresets.json`，不需要改共享的 `CMakePresets.json`。

## 相关文档

* [产品需求文档](/D:/UGit/ai_sdk/docs/PRD.md)
* [跨平台构建方案](/D:/UGit/ai_sdk/docs/跨平台构建方案.md)

## 当前状态

当前仓库已经不是单纯的目录骨架，已经具备以下能力：

* 统一的根目录构建入口
* 模块化的 `include/...` 头文件结构
* DeepSeek 真实 API 的普通请求与流式请求
* 基于 `.env` 和环境变量的本地配置读取
* Tool Schema 请求序列化、Tool Call 响应解析
* 本地工具注册、单批串行执行和 Tool 结果消息转换
* 显式、线程安全、默认脱敏的内存 Trace 与 JSON 导出
* 独立 MCP Client、stdio / Streamable HTTP 传输和显式 Tool Adapter
* 可本地执行的核心测试与在线 Provider 测试入口

下一步的实现重点会是：

* 更完整的工具参数 Schema 校验
* 流式 Tool Call 增量聚合
* `MiniMaxProvider`
* Trace 持久化与 OpenTelemetry 适配（当前 Trace 只存在于内存中，支持 snapshot() 和 toJson()；进程退出后数据就消失，也不会发送给外部监控系统。）
* MCP Resources、Prompts 与完整 OAuth 2.1 支持
* Agent 层的 Skill Loader 与 Skill Registry；Skill 不并入 MCP 协议模块
