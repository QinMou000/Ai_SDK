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

  tests/
    smoke/
    core/
    provider/
    tool/
    http/
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
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat
cmake --preset local-windows-debug
cmake --build --preset local-windows-debug -v
ctest --preset local-windows-debug
```

方式 B：一次性执行

```cmd
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && cmake --preset local-windows-debug
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && cmake --build --preset local-windows-debug -v
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat && ctest --preset local-windows-debug
```

### Release 构建

```cmd
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat
cmake --preset local-windows-release
cmake --build --preset local-windows-release -v
```

### Windows 快速验证

如果只想验证核心库和依赖是否能正确配置，可以使用 bootstrap 预设：

```cmd
call D:\software\VS\Community\VC\Auxiliary\Build\vcvars64.bat
cmake --preset local-windows-bootstrap
cmake --build --preset local-windows-bootstrap -v
```

这个预设会关闭 `examples` 和 `tests`，只验证核心库入口，适合排查工具链问题。

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
  "timeout_ms": 30000
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
* 可本地执行的核心测试与在线 Provider 测试入口

下一步的实现重点会是：

* 更完整的工具参数 Schema 校验
* 流式 Tool Call 增量聚合
* `MiniMaxProvider`
* `trace` 可观测链路
