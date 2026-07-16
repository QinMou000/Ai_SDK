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
* 位于 `AIClient` 上层的同步 `SimpleAgent`，提供最小 ReAct 循环与受限工作区文件工具

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
    agent/

  src/
    AIClient.cpp
    core/
    provider/
    http/
    tool/
    trace/
    agent/

  examples/
    01_chat_deepseek/
    02_chat_minimax/
    03_stream_chat/
    04_register_tool/
    05_tool_call/
    06_simple_agent/
    07_trace/

  tests/
    smoke/
    core/
    provider/
    tool/
    http/
    trace/
    agent/
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
* `examples/06_simple_agent`
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

### SimpleAgent ReAct 示例

`SimpleAgent` 是 `AIClient` 之上的可复用同步编排层，不修改 `AIClient` 的请求、Provider 或传输语义。每次 `run(...)` 都从独立的系统消息和用户消息开始：模型返回 Tool Call 时，Agent 执行工具并回填 assistant/tool 消息；模型不再返回 Tool Call 时，当前文本即为最终答案。为防止异常循环，内部最多发起 16 次模型请求，但调用方不需要也不能传入循环轮次。

配置 `DEEPSEEK_API_KEY` 后，可运行在线示例：

```powershell
.\build\windows-debug\examples\06_simple_agent\example_simple_agent.exe "请查询当前时间，并计算 12.5 加 7.5"
```

示例注册两个业务 `Low` 工具：`get_current_time` 与 `add_numbers`；同时把**启动时的当前工作目录**作为显式工作区根目录，注册以下七个 `Low` 文件工具：

* `list_directory`：列出单层目录条目。
* `read_text_file`：读取 UTF-8 文本文件。
* `create_text_file`：只新建不存在的文本文件。
* `write_text_file`：只覆盖已经存在的文本文件。
* `replace_text_in_file`：只精确替换唯一的一处文本。
* `find_files`：递归查找文件名包含指定文本的普通文件。
* `search_text`：递归检索 UTF-8 文本，返回文件相对路径、行号、列号和文本预览。

文件路径必须相对工作区；工具拒绝绝对路径、越界路径、符号链接逃逸、`.git`、`.env`、常见私钥以及非 UTF-8 文本。单个文件和写入内容最大为 64 KiB，目录列表最多返回 256 项；两项搜索默认最多返回 256 个命中，也可在单次调用中降低 `max_results`。`search_text` 会跳过超限或非 UTF-8 文件，并在结果中标记跳过数量和截断状态。生产调用应传入最小必要的专用目录；若不在 `SimpleAgentOptions` 中提供 `WorkspaceFileToolOptions`，Agent 不会注册任何文件工具。

```cpp
AIClient client(config);
client.tools().registerTool(low_risk_tool, handler);

SimpleAgentOptions options;
options.workspace_file_tools = WorkspaceFileToolOptions{"D:/agent-workspace"};
SimpleAgent agent(client, std::move(options));

TraceSession trace = client.startTrace();
AgentResult result = agent.run("整理工作区中的说明文件", trace);
```

Agent 每轮只向模型展示并自动执行 `ToolRiskLevel::Low` 工具。模型臆造的 `Medium` 或 `High` 工具不会执行，拒绝结果会作为 Tool 消息回填给模型。工具失败和未知工具同样会回填，模型可以选择修复参数、改用其他工具或直接解释失败。

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
* `tests/agent/ai_sdk_agent_test`：离线覆盖多轮 ReAct、独立任务、工具失败恢复、风险拦截、熔断、Trace 与工作区文件边界。

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
* 无会话状态的同步 `SimpleAgent`、低风险工具策略与受限工作区文本工具
* 可本地执行的核心测试与在线 Provider 测试入口

下一步的实现重点会是：

* 更完整的工具参数 Schema 校验
* 流式 Tool Call 增量聚合
* Agent 会话管理、短期/长期记忆与按工具审批策略
* `MiniMaxProvider`
* Trace 持久化与 OpenTelemetry 适配（当前 Trace 只存在于内存中，支持 snapshot() 和 toJson()；进程退出后数据就消失，也不会发送给外部监控系统。）
