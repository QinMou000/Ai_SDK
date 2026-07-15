# MCP Client 可选扩展 PRD

## 0. 文档信息

| 项目 | 内容 |
|---|---|
| 任务目录 | `.trellis/tasks/07-14-mcp-client` |
| 文档状态 | 已确认，进入实现 |
| 编写日期 | 2026-07-14 |
| 确认日期 | 2026-07-15 |
| 目标协议 | MCP `2025-11-25` |
| 首版能力 | MCP Client、基础生命周期、本地 `stdio`、远程 Streamable HTTP、Tools |
| 明确排除 | Skill、MCP Server、Agent Loop、Resources、Prompts 等非 Tools 能力 |

本 PRD 定义 MCP Client 的产品边界与实现合同；用户已确认整份需求，可以进入 Trellis 实现阶段。

## 1. 背景与问题

当前 SDK 已具备模型 Provider、Tool 定义、`ToolRegistry`、`ToolExecutor` 和显式 Tool Call 执行能力，但工具来源仅限调用方在进程内注册的 C++ 处理函数。若应用希望复用外部 MCP Server，目前必须自行处理子进程、JSON-RPC、初始化协商、工具发现、工具调用、超时、错误和结果适配，重复成本较高，也容易把协议细节耦合进 Agent。

本任务需要在 SDK 层提供可复用的 MCP 协议能力，同时保持现有职责边界：

- SDK 负责可靠地连接一个调用方明确配置的 MCP Server，并把其 Tools 适配为现有工具合同。
- Agent 或应用负责决定连接哪些 Server、向模型暴露哪些工具、何时审批与执行，以及是否继续下一轮模型调用。
- Skill 仍属于 Agent 层的知识与工作流加载机制，不通过本任务实现，也不与 MCP Client 建立隐式依赖。

## 2. 产品目标

### 2.1 核心目标

新增独立、可选的 `ai_sdk_mcp` 模块，使调用方能够：

1. 显式启动并初始化一个受信任的本地 MCP `stdio` Server，或连接一个受信任的远程 Streamable HTTP Endpoint。
2. 对 Streamable HTTP 同时支持普通 JSON 响应、POST SSE、GET SSE、会话管理与有限断线恢复。
3. 分页发现 Server 提供的工具，并保留 MCP 工具的完整元数据。
4. 同步调用 MCP 工具，并区分协议错误、传输结果未知与工具业务失败。
5. 将调用方选定的 MCP 工具安全地适配为 `Tool + ToolHandler`。
6. 将适配结果显式注册到现有 `ToolRegistry`，继续复用 `AIClient::executeToolCalls(...)` 和 `ToolExecutor`。
7. 在不启用或不链接 MCP 模块时，让现有核心 SDK 行为与测试保持不受影响；唯一核心 API 增量是与 MCP 类型无关的 `ToolRegistry` 通用注销能力。

### 2.2 成功指标

- 所有 MCP 必测路径完全由本地自动化测试覆盖；`stdio` 使用本地子进程夹具，HTTP 使用本地回环 Server，不依赖公网、API Key、人工 Server、Node 或 Python 测试夹具。
- Windows MSVC 与 Linux GCC 均能完成 MCP 开启、MCP 关闭两套构建验证。
- 多 Server 或本地工具重名不会被 `ToolRegistry` 静默替换。
- Server 超时、提前退出、输出非法协议或拒绝工具调用时，不会导致宿主进程崩溃、无限等待或遗留子进程。
- MCP 代码对核心 `ai_sdk` 保持单向依赖；核心库不反向引用 MCP。

## 3. 非目标

首版明确不实现以下内容：

- Skill Loader、Skill Registry、`SKILL.md` 扫描、解析、选择、组合与执行。
- MCP Server 实现或 Server 开发框架。
- 已经废弃的 `2024-11-05` HTTP + SSE 传输及其兼容回退。
- MCP Resources、Prompts、Sampling、Elicitation、Roots、Tasks 和服务端日志 UI。
- 自动扫描系统中已经安装的 MCP Server。
- 由模型生成或修改 Server 启动命令、参数、环境变量或工作目录。
- SDK 内置 Agent Loop、Planner、Memory、多 Agent 编排、自动重试或自动补发模型请求。
- 自动把 Server 的全部工具暴露给模型，或绕过上层审批执行工具。
- 根据 Server 提供的 `annotations` 自动降低工具风险等级。
- MCP 工具列表变更后的注册表热更新与自动注销。
- MCP 富媒体内容直接转换为现有 `ToolResult` 的无损模型消息。
- 并发、多路复用或乱序完成多个 `tools/call` 请求。
- SDK 内置 Client Pool、连接租借、负载均衡和跨会话自动重试；需要并行时由上层管理多个 Client。
- JSON-RPC Batch；每次 HTTP POST 只发送一条 JSON-RPC 消息。
- MCP OAuth 2.1 浏览器授权流程，包括 Protected Resource Metadata、OAuth/OIDC 发现、PKCE、客户端注册、回调监听、Token 存储与刷新、增量 Scope。
- 自定义 CA Bundle、mTLS 客户端证书、HTTP(S) 代理、代理认证和关闭 TLS 校验的逃生选项。
- macOS 平台进程实现；首版支持范围为 Windows 与 Linux。

## 4. 已确认的现有工程事实

实现前已分析以下现有模式，后续设计必须复用其约束，而不是另建平行体系。

| 现有实现或模式 | 代码证据 | 对 MCP 的约束 |
|---|---|---|
| 窄传输接口与测试注入 | `include/http/HttpClient.h`、`src/http/HttpClient.cpp`、`tests/trace/trace_integration_test.cpp` | 新增独立 `IMCPTransport`；生产实现隐藏在 `.cpp`，测试注入脚本传输。`IHttpTransport` 只作为设计参考，既不能承载长连接双向 `stdio`，也缺少 Streamable HTTP 所需的 GET、DELETE、响应头、会话与恢复语义。 |
| Provider 专用 SSE | `include/http/SSEParser.h`、`src/http/SSEParser.cpp`、`src/provider/DeepSeekProvider.cpp` | 现有解析器只处理完整 DeepSeek 事件并丢弃 `id`、`retry` 等字段；MCP 必须新增跨网络分块的有状态 SSE 解码器，不能复用或扩展 Provider 解析器。 |
| Tool 定义与执行边界 | `include/tool/Tool.h`、`include/tool/ToolRegistry.h`、`include/tool/ToolExecutor.h` | MCP 工具最终适配为 `Tool + ToolHandler`；SDK 不增加隐式 Agent Loop。 |
| 注册表同名替换语义 | `src/tool/ToolRegistry.cpp` | Adapter 必须在注册前完成命名、Schema 和冲突预检，禁止 MCP 工具静默覆盖现有工具。 |
| 注册表缺少注销语义 | `include/tool/ToolRegistry.h`、`src/tool/ToolRegistry.cpp` | 为处理已确认的目录版本失效，本任务给通用注册表增加显式批量注销；API 不依赖 MCP 类型，线程安全合同保持不变。 |
| 同步串行工具执行 | `src/tool/ToolExecutor.cpp` | 首版公开 API 保持同步，单连接最多一个在途请求，不提前引入并发执行合同。 |
| 分层、确定性的 GTest | `tests/CMakeLists.txt`、`tests/tool/`、`tests/trace/trace_integration_test.cpp` | 新增 `tests/mcp/`；协议使用假传输测试，真实 `stdio` 使用仓库内 C++ 子进程夹具，真实 HTTP 使用动态端口回环 Server，不依赖公网、固定端口和任意休眠。 |
| 显式 Trace 上下文 | `include/trace/Trace.h`、`include/trace/TraceRecorder.h` | 首版复用现有 `ToolExecution` Trace，不伪造 MCP 子步骤，不使用全局或线程局部 Trace 上下文。 |
| Provider 专用顶层配置 | `include/core/Config.h` | MCP Server 使用独立 `MCPServerConfig`，不向 Provider `Config` 塞入 MCP 字段。 |
| CMake 与依赖基线 | `CMakeLists.txt`、`vcpkg.json`、`CMakePresets.json` | C++17；公开头文件不泄露平台类型；首版复用 `nlohmann/json` 和 `Threads`，不新增 Boost 等依赖。 |

适用项目规范：

- `.trellis/spec/backend/directory-structure.md`
- `.trellis/spec/backend/sdk-contracts.md`
- `.trellis/spec/backend/error-handling.md`
- `.trellis/spec/backend/logging-guidelines.md`
- `.trellis/spec/backend/trace-contracts.md`
- `.trellis/spec/backend/quality-guidelines.md`
- `.trellis/spec/guides/cross-layer-thinking-guide.md`
- `.trellis/spec/guides/code-reuse-thinking-guide.md`

协议与 C++ 选型依据见[研究记录](./research/mcp-protocol-and-cpp-options.md)。

## 5. 角色与职责边界

| 层级 | 必须负责 | 明确不负责 |
|---|---|---|
| `ai_sdk_mcp` | MCP 生命周期、JSON-RPC 关联、`stdio` 与 Streamable HTTP、HTTP 会话与 SSE 恢复、逐请求凭据注入、分页、超时、关闭、完整结果模型、Tool Adapter | Token 获取、刷新与保存、Server 自动发现、模型循环、Skill |
| 核心 `ai_sdk` | 模型请求、`ToolRegistry`、`ToolExecutor`、既有 Trace | 感知或持有 MCP Client、自动启动 Server |
| Agent / 应用 | Server 配置来源、Client 生命周期、工具筛选、别名、风险覆盖、审批、消息回填、下一轮模型调用 | 重复实现 MCP 线协议 |
| MCP Server | 声明并执行远端工具 | 决定应用是否向模型暴露或执行工具 |
| Skill 系统（未来任务） | 读取知识与工作流文件、向 Agent 提供上下文与操作指引 | 由本 MCP 模块解析或自动加载 |

## 6. 依赖与集成点

```text
调用方 / Agent
  ├─ 按 server_id 持有多个 MCPClient
  │    ├─ MCPClient A：一个 Server 配置 + 一个逻辑 MCP 会话
  │    ├─ MCPClient B：另一个 Server 配置 + 另一个逻辑 MCP 会话
  │    └─ 每个 MCPClient 持有一个 IMCPTransport
  │         ├─ StdioMCPTransport
  │         │    └─ 受信任的本地 MCP Server 子进程
  │         └─ StreamableHttpMCPTransport
  │              ├─ POST：JSON 或 SSE 响应
  │              ├─ GET：Server 消息监听与 SSE 恢复
  │              └─ 受信任的远程 MCP Endpoint
  │
  ├─ MCPClient::listTools()
  │    └─ MCPToolAdapter::adaptTools(...)
  │         └─ MCPToolBinding { Tool, ToolHandler }
  │              └─ 调用方显式注册到 ToolRegistry
  │
  └─ Agent Loop（上层自行编排）
       ├─ ToolRegistry::listTools() -> ChatRequest.tools
       ├─ AIClient::chat(...) -> 模型返回 ToolCall
       ├─ 上层筛选 / 审批
       ├─ AIClient::executeToolCalls(...)
       │    └─ ToolHandler -> MCPClient::callTool(...)
       └─ 上层决定是否回填消息并再次请求模型

Skill Loader：不在本依赖图内，未来由 Agent 通过独立文件工具或专用组件调用。
```

关键集成合同：

- `ai_sdk_mcp` 可以依赖 `ai_sdk`；`ai_sdk` 不得包含 MCP 头文件或链接 `ai_sdk_mcp`。
- `AIClient` 不新增 MCP 成员、Server 配置或自动注册入口。
- MCP Adapter 只产生现有工具合同；模型 Provider 不感知工具来自本地还是 MCP。
- 构造函数不得执行 I/O；进程启动与协议初始化只发生在显式 `connect()` 中。
- 一个 `MCPClient` 对应一个确定的 `server_id`、一个传输配置和一个逻辑 MCP 会话。多 Server 场景由 Agent 或应用持有多个 Client，不在 `AIClient` 内建立隐式全局管理器。
- 同一 Server 的多个 Client 代表彼此独立的进程或 HTTP 会话；首版不提供连接池，也不把创建多个 Client 当成 GET SSE 的必要条件。

## 7. 用户场景

### 7.1 正常接入

1. 应用从自身可信配置读取 Server 可执行文件、参数、环境变量与工作目录。
2. 应用创建 `MCPClient` 并调用 `connect()`。
3. Client 完成 `initialize`、能力协商和 `notifications/initialized`。
4. 应用调用 `listTools()`，按自己的白名单筛选工具。
5. 应用为工具设置必要的本地别名和风险覆盖，并调用 Adapter。
6. 应用在注册前展示或审计最终工具清单，然后显式注册到 `ToolRegistry`。
7. 模型返回 Tool Call 后，上层完成审批，再通过现有执行链路调用远端工具。
8. 应用不再使用 Server 时显式调用 `close()`；析构只承担兜底清理。

### 7.2 Server 异常

- 初始化版本不兼容或未声明 `tools`：连接失败，不进入 Ready。
- stdout 出现日志、非法 JSON 或错误响应 ID：判定协议违规，连接进入 Faulted，等待中的调用确定失败。
- stderr 有输出：持续消费，但不得将其等同于协议失败。
- Server 提前退出或关闭 stdout：当前请求失败，Client 进入 Faulted，并完成进程回收。
- 工具返回 `isError: true`：Client 返回合法的业务失败结果，不抛成 JSON-RPC 异常。
- 请求超时：发送取消通知并停止等待；未提交的操作返回可分类的超时错误，已提交但未收到终局响应的工具调用返回带 `RequestTimeout` 根因的 `OutcomeUnknown`；初始化请求不发送取消。

### 7.3 远程 Streamable HTTP

- 应用从可信配置提供固定 MCP Endpoint，并创建 HTTP 传输配置。
- 初始化 POST 同时接受 `application/json` 与 `text/event-stream` 响应；若 Server 分配会话 ID，Client 安全保存并自动附加到后续请求。
- 初始化完成后默认尝试建立一条 GET SSE 监听流，用于接收会话空闲期的 Server 请求、通知与保活消息；Server 返回 `405 Method Not Allowed` 时正常降级为仅处理 POST JSON/SSE，不影响其他 MCP 功能。
- POST SSE 或 GET SSE 断开且存在事件 ID 时，Client 通过 GET + `Last-Event-ID` 有限恢复；恢复只续接响应流，绝不重新 POST 原始工具调用。
- 正常关闭时，若存在 HTTP 会话则尽力发送 DELETE；`405` 表示 Server 不支持主动删除，仍可完成本地关闭。

### 7.4 生命周期失效

- Client 已关闭或销毁后，已注册的弱引用 Handler 返回稳定失败，不延长 Server 进程生命周期。
- 一个 Client 实例关闭或进入 Faulted 后不支持重新连接；调用方需要创建新实例。
- 重复 `close()` 是幂等操作；其他错误状态调用按错误合同处理。

## 8. 功能需求

### 8.1 模块与构建

| 编号 | 需求 |
|---|---|
| MCP-FR-001 | 新增 CMake 选项 `AISDK_BUILD_MCP`，首版默认 `ON`，允许调用方显式设为 `OFF`。 |
| MCP-FR-002 | 新增独立目标 `ai_sdk_mcp`；它 `PUBLIC` 依赖 `ai_sdk` 与 `nlohmann_json::nlohmann_json`，`PRIVATE` 显式依赖 `cpr::cpr` 与 `Threads::Threads`。 |
| MCP-FR-003 | `AISDK_BUILD_MCP=OFF` 时不得配置、编译或链接任何 MCP 源文件；通用 `ToolRegistry` 注销 API 仍属于核心，但既有方法语义与现有测试保持不变。 |
| MCP-FR-004 | Windows 与 Linux 进程代码使用条件源文件；公共头文件不得出现 `HANDLE`、文件描述符或平台宏分支。 |
| MCP-FR-005 | 首版不新增 vcpkg 依赖；Streamable HTTP 复用已有 cpr，但不依赖仓库未锁定版本才具备的高层 SSE API，也不引入 Boost.Process、Node、Python 或社区 C++ MCP SDK。 |
| MCP-FR-006 | CMake 配置期必须直接校验构建图：ON 时 `ai_sdk_mcp -> ai_sdk` 且 `ai_sdk` 的 `LINK_LIBRARIES` / `INTERFACE_LINK_LIBRARIES` 不得出现 `ai_sdk_mcp`；OFF 时不得存在 `ai_sdk_mcp` 目标，核心目标源文件不得包含 `src/mcp/`。违反任一不变量必须使 CMake 配置失败。 |

建议目标拆分：

| 目标 | 职责 |
|---|---|
| `ai_sdk` | 现有核心 SDK；只新增不依赖 MCP 类型的 `ToolRegistry` 单项/批量注销能力 |
| `ai_sdk_mcp` | MCP 生命周期、协议、`stdio`、Streamable HTTP、SSE 与 Tool Adapter |
| `ai_sdk_mcp_test` | 协议、Client 和 Adapter 离线单元测试 |
| `ai_sdk_mcp_test_server` | 可脚本化的本地 C++ `stdio` Server 夹具 |
| `ai_sdk_mcp_stdio_test` | 真实子进程集成测试 |
| `ai_sdk_mcp_http_test_server` | 动态端口的本地 C++ HTTP/1.1 回环夹具 |
| `ai_sdk_mcp_tls_test_server` | 仅测试构建的本地 C++ TLS 回环夹具；Windows 使用 SChannel，Linux 使用 cpr `[ssl]` 依赖图已提供的 OpenSSL |
| `ai_sdk_mcp_http_test` | 真实 cpr、JSON/SSE、会话、恢复和取消集成测试 |
| `example_mcp_tool_call` | 显式连接、筛选、注册与执行示例 |

TLS 夹具不改变“不新增 vcpkg 依赖”合同：Windows 测试目标只链接系统 `Secur32`、`Crypt32` 与 `Ws2_32`，Linux 测试目标链接已由 cpr SSL 依赖图提供的 `OpenSSL::SSL`。CMake 配置期必须校验对应平台能力；若当前 vcpkg 依赖图在 Linux 不再提供 OpenSSL，任务保持未完成并先修订技术选型，不允许隐式使用未锁定的系统命令或跳过 TLS 测试。

### 8.2 Server 配置

新增独立 `MCPServerConfig`，由公共 `server_id`、公共限制和且仅一个传输变体组成：

```text
MCPServerConfig
  ├─ server_id
  ├─ 请求、关闭、消息队列和工具目录公共限制
  └─ transport：二选一
       ├─ MCPStdioServerConfig
       └─ MCPStreamableHttpConfig
```

公共字段至少包含：

| 字段 | 合同 |
|---|---|
| `server_id` | 应用内稳定且非敏感的 Server 标识；用于命名空间和安全错误定位。不能为空。 |
| `request_timeout` | 单个 JSON-RPC 请求段的默认超时，默认 30 秒，必须为正值；起止和 SSE 例外见本节计时合同。 |
| `absolute_request_timeout` | 用户发起的 `connect()`、`ping()`、完整分页 `listTools()` 与 `callTool()` 的绝对上限，默认 2 分钟；凭据获取、分页、SSE 事件、退避或重连都不重置它。后台 GET SSE 不使用此上限。 |
| `close_timeout` | `close()` 停止前后台任务并释放本地资源的总上限，默认 5 秒；超限时执行强制清理但不抛异常。 |
| `max_message_bytes` | 单条 JSON-RPC 消息上限，默认 8 MiB。 |
| `max_pending_messages` | 两种传输共用的协议消息队列上限，默认 64 条。请求、响应或不可合并通知入队失败时进入 `Faulted`，禁止静默丢弃。 |
| `max_error_text_bytes` | 进入公开异常或 `ToolResult` 的 UTF-8 错误文本上限，默认 4096 字节；超出部分截断并追加固定中文标记。 |
| `max_pages` | 一次 `tools/list` 最大页数，默认 100。 |
| `max_tools` | 一次完整列举最大工具数，默认 4096。 |

`MCPStdioServerConfig` 至少包含：

| 字段 | 合同 |
|---|---|
| `executable` | 真实可执行文件的绝对路径；不接受一整段 shell 命令。 |
| `arguments` | 参数数组，逐项传入子进程，不进行 shell 拼接或二次解释。 |
| `working_directory` | 可选绝对工作目录；缺失时使用应用明确选择的目录，不隐式扫描。 |
| `environment` | 显式环境变量映射；值属于敏感数据，不进入 Trace、异常或默认日志。 |
| `inherit_parent_environment` | 默认 `false`；需要继承时必须由应用显式开启。 |
| `startup_timeout` | 初始化总超时，默认 10 秒，必须为正值。 |
| `shutdown_timeout` | 正常关闭等待时间，默认 2 秒；随后进入终止与强制终止阶段。 |
| `max_stderr_tail_bytes` | 可选诊断尾部缓存上限，默认 64 KiB；默认不对外输出。 |

`MCPStreamableHttpConfig` 至少包含：

| 字段 | 合同 |
|---|---|
| `endpoint` | 固定且受信任的 MCP Endpoint；构造后不可由模型、Tool 结果或 Server 响应修改。 |
| `connect_timeout` | 每个 HTTP 尝试从开始 DNS/TCP/TLS 到连接建立的上限，默认 10 秒；它不覆盖响应正文。 |
| `stream_idle_timeout` | SSE 无字节活动超时，默认 30 秒；与绝对请求上限分开。 |
| `credential_timeout` | 单次凭据 Provider 调用的协作式上限，默认 2 秒，必须为正值且不大于 `close_timeout`。 |
| `max_sse_event_bytes` | 单个 SSE 事件上限，默认 8 MiB，且不得大于消息上限。 |
| `max_event_id_bytes` | 单个 SSE 事件 ID 上限，默认 4096 字节；超限视为协议违规。 |
| `max_session_id_bytes` | `MCP-Session-Id` 上限，默认 1024 字节，并且每个字节都必须是可见 ASCII。 |
| `max_reconnect_attempts` | 单条 SSE 流连续恢复次数上限，默认 3 次；成功建流并收到合法事件后重置连续失败计数。 |
| `max_sse_retry_delay` | Server `retry` 的本地等待上限，默认 30 秒；更大值按该上限等待并记录协议偏差，负值或非整数视为协议违规。 |
| `allow_loopback_http` | 默认 `false`；仅开发测试可显式允许字面量 `127.0.0.0/8` 或 `::1`。首版不接受 `localhost`，避免 DNS 解析漂移。 |
| `credential` | 可选闭合凭据配置：匿名、Bearer 或固定 Header 三选一；应用负责秘密获取、刷新和保存，SDK 不内置 OAuth。 |

计时合同统一使用可注入的单调时钟，不使用可回拨的系统时间：

| 阶段 | 计时起点 | 计时终点 / 重置 | 包含关系 |
|---|---|---|---|
| 前台绝对上限 | 公开操作成功取得前台槽且完成同步参数校验时 | 操作返回或超时；永不重置 | 包含凭据获取、进程/网络建立、所有分页、SSE 读取、退避和恢复；`ClientBusy` 在取得槽之前返回，不启动计时 |
| stdio `startup_timeout` | 开始创建子进程 | 完成初始化并进入 Ready，或超时 | 与 `connect()` 的绝对上限同时运行，先到者生效 |
| HTTP `connect_timeout` | 每个 POST/GET/DELETE 尝试开始解析和建连 | 完成 DNS/TCP/TLS 建连或超时 | 它是当前 `request_timeout` 或 `close_timeout` 内的子上限，不与外层上限串行相加 |
| 普通 JSON / stdio 请求段 | 请求原子提交发送时 | 完整匹配的终局 JSON-RPC 响应到达，或超时 | `listTools()` 每一页重新启动段计时，但整个分页操作仍受同一绝对上限限制 |
| HTTP POST JSON | POST 原子提交时 | 响应头和有界 JSON 正文全部读取，或超时 | 连接子阶段同时受 `connect_timeout` 限制 |
| HTTP POST/GET SSE 建流 | 该 HTTP 尝试开始 | 收到并校验响应头，或超时 | 建流后该段的 `request_timeout` 结束，转由 SSE 空闲与外层上限约束 |
| SSE 空闲上限 | 成功建立 SSE 流时 | 每收到一个网络字节重置；到期时视为断线并进入恢复 | 注释和空事件可重置空闲上限，但不重置前台绝对上限 |
| SSE 退避 / 恢复 | 流意外中断或空闲超时 | 恢复成功、次数耗尽或外层上限到期 | 每个恢复 GET 都重新获取凭据，并有自己的 `connect_timeout` 与建流 `request_timeout`；前台退避等待计入绝对上限，后台 Listener 不设绝对寿命 |
| `close_timeout` | `close()` 成功将状态切换为 Closing 时 | 所有可回收资源释放或总上限到期 | 包含取消、正在执行的凭据回调剩余时间、DELETE、终止进程与线程收敛；任一子阶段不得延长总上限 |

通用配置校验失败必须在启动进程或发送网络请求前抛出 `std::invalid_argument`。`stdio` 首版不直接执行 `.cmd`、`.bat`、shell 管道、重定向或复合命令；此类 Server 应由应用显式指定真实解释器或可执行程序及参数。

### 8.3 传输接口

新增平台无关的 `IMCPTransport` 窄接口，职责只限于：

- 根据已经校验的传输配置建立本地进程通道或远程 HTTP 通道。
- 通过“准备请求 + 原子提交”两阶段发送一条完整、紧凑的 UTF-8 JSON-RPC 消息，并把收到的单条协议消息投递给 `MCPClient`。
- 传递 EOF、HTTP 状态、会话失效、响应结果未知和不可恢复传输错误等闭合事件。
- 在初始化成功后接收协商协议版本，使 HTTP 传输能够附加版本头。
- 在 `close()` 中停止所有读取、请求、重连和后台线程，并按传输类型释放资源。
- `prepareMessage()` 在 Client 状态锁外完成序列化与本次 HTTP 凭据获取，形成不可变、可销毁的 `PreparedMCPMessage`；该阶段不得启动 MCP 网络/进程 I/O，也不得进入发送队列。
- `commitPrepared()` 只允许在 Client 持有状态锁并完成二次校验后调用；它不得调用用户代码或执行阻塞网络 I/O，只把准备请求原子放入有界发送队列。前台请求与后台控制消息可以并发准备，但每条消息必须完整入队且不会发生字节交错。
- 锁顺序固定为 Client 状态锁先于 Transport 出站队列锁；`commitPrepared()` 不得反向回调 Client。Transport 的物理发送 Worker 不持有 Client 状态锁。

接口不得解释 Tools 方法、请求 ID、JSON-RPC 结果或业务错误。协议关联、状态机和 Schema 校验属于 `MCPClient`；换行分帧、HTTP 方法、响应头、SSE 和 HTTP 会话属于具体传输。

两阶段合同适用于 stdio 与 HTTP：stdio 的准备阶段只形成待写入字节，HTTP 的准备阶段还形成固定 Method、URL、正文、非秘密头和当前凭据快照。Client 在准备前捕获与该消息类型有关的状态代次；Provider 返回后必须在同一状态锁下按下表二次校验，再调用 `commitPrepared()`。准备期间发生失效时，准备请求及秘密立即销毁，保证零 MCP 网络写入。

| 准备消息类型 | 提交前必须复核 | 与提交无关、不得阻断的变化 |
|---|---|---|
| `tools/call` | Ready、会话与 Transport 代次、Catalog 身份/代次/目标工具、Listener 安全状态 | 无 |
| `tools/list` 每一页 | Ready、会话与 Transport 代次、本轮起始目录代次、Listener 安全状态 | 已收集但尚未发布的临时页只由本轮代次管理 |
| 公开 `ping` | Ready、会话与 Transport 代次 | `list_changed`、Catalog 代次和 Listener 目录 stale |
| `initialize` / `notifications/initialized` | 当前 Connecting / Initializing 阶段、Transport 代次，以及已经取得时的会话代次 | 工具目录尚未发布，不校验 Catalog |
| Server 请求响应，包括 `ping` / `-32601` | 主状态尚允许协议响应、会话与 Transport 代次 | `list_changed`、Catalog 代次和 Listener 目录 stale；目录变化不得阻断必需响应 |
| `notifications/cancelled` | 原请求仍处于待取消状态、会话与 Transport 代次 | Catalog 与 Listener 目录状态；原请求已终局时可在提交前直接丢弃该尽力通知 |
| 首次 Listener GET | Initializing + Starting、会话与 Transport 代次 | Catalog 尚未发布；`connect()` 必须等该 GET 的 `200/405` 分类后才进入 Ready |
| 后台 Listener 恢复 GET | Ready + Recovering、会话、Transport 与 Listener 流代次 | Catalog stale；监听恢复正是重新建立目录安全性的前提 |
| 前台 POST 响应恢复 GET | 父请求仍在途、父请求与响应流代次、会话和 Transport；主状态继承父请求，`initialize` 为 Initializing，Ready 公开请求保持 Ready | 父 `tools/call` 已 Submitted 后发生的 Catalog stale 不取消结果恢复；只阻止新工具请求 |
| DELETE | Closing、会话与 Transport 代次 | Catalog 与 Listener 状态 |

`close()` 先切换 Closing 或会话失效会取消所有尚未提交的消息；根因分别为 `OperationCancelled` 与 `SessionExpired`。`list_changed` 或 Listener 目录不安全只取消依赖目录的 `tools/call` / `tools/list`，根因为 `ToolCatalogStale`，不得阻断公开 `ping`、Server 请求响应、取消通知、Listener 恢复或关闭清理。

HTTP 实现内部允许增加不公开的 `IMCPHttpBackend` 测试缝，用 SDK 自有请求、响应与取消类型表达 POST、GET、DELETE、响应头、普通正文和增量正文。cpr 类型必须封闭在 `.cpp`。

#### 8.3.1 stdio 合同

生产实现 `StdioMCPTransport` 必须满足：

- stdin/stdout 使用逐行 UTF-8 紧凑 JSON；发送统一使用 LF，接收兼容 LF 与 CRLF。
- stdout 上的空行、日志或其他非 JSON-RPC 文本均视为协议违规；stderr 不进入协议解析。
- EOF 前没有换行的残缺尾部不得作为完整消息执行。
- Windows 使用宽字符进程 API、受控句柄继承和 Job Object；Linux 使用直接进程启动、受控文件描述符继承和独立进程组。
- Linux 写入已关闭管道时处理 `EPIPE`，不得让 `SIGPIPE` 终止宿主进程。
- 所有 stdin 写入经过唯一写锁或单写者队列串行化；前台调用、Server 请求响应和取消通知不得交错成无效 JSON 行。
- 多次启动和关闭后不得泄漏线程、句柄、文件描述符、僵尸进程或子孙进程。

#### 8.3.2 Streamable HTTP 合同

生产实现 `StreamableHttpMCPTransport` 必须满足：

- 每条客户端 JSON-RPC 消息使用一个新的 HTTP POST，请求体只包含一条消息。
- POST 固定发送 `Content-Type: application/json; charset=utf-8`，并声明 `Accept: application/json, text/event-stream`。
- JSON-RPC 请求的响应必须同时支持 `application/json` 单对象与 `text/event-stream` 增量流。
- 客户端通知或对 Server 请求的响应 POST，规范成功值为 `202 Accepted` 空正文；兼容接受 `204 No Content`，但只记为有界协议偏差诊断。
- 初始化成功后，所有后续 POST、GET 和 DELETE 都发送 `MCP-Protocol-Version: 2025-11-25`。
- 初始化响应若带 `MCP-Session-Id`，必须校验为可见 ASCII，按秘密保存，并附加到所有后续 POST、GET 和 DELETE。
- 初始化完成后默认尝试一条 GET SSE 监听流；请求声明 `Accept: text/event-stream`，用于接收会话空闲期的 Server 请求、通知与保活消息。`connect()` 等待首次 GET 返回响应头：`200 + text/event-stream` 或 `405` 才能成功进入 Ready。
- GET 返回 `405 Method Not Allowed` 表示 Server 不提供独立监听流，不视为连接失败；Client 停止 GET 相关重连，正常降级为仅处理 POST JSON/SSE，其他 MCP 功能保持可用。
- 首次 GET 的 `401/403` 返回 `AuthenticationRequired`，带会话 ID 的 `404` 返回 `SessionExpired`，非法 Content-Type、其他 `4xx`、`5xx`、建连失败或首次响应头超时都使 `connect()` 失败并进入 `Faulted`；首版不在初始化路径隐藏重试。
- GET SSE 上收到 `ping` 请求时，Client 通过独立 POST 返回 JSON-RPC 响应；未声明能力对应的其他请求返回 `-32601`。合法未知通知可以忽略，`notifications/tools/list_changed` 只触发目录过期信号。
- SSE 注释、空 data 预热事件和其他合法保活帧只刷新流活动时间或游标，不进入 JSON-RPC 业务分发。
- HTTP 正常关闭时，存在会话 ID 则发送 DELETE；任意 `2xx` 表示成功，`404` 表示会话已不存在，`405` 表示不支持主动删除，三者都允许本地关闭完成。
- 不支持旧 HTTP + SSE 探测与回退，不跟随 `400/404/405` 去寻找旧 `endpoint` 事件。
- 每个生产 cpr Session 必须显式把 libcurl 代理设为空字符串，以禁用 `http_proxy`、`HTTP_PROXY`、`https_proxy`、`HTTPS_PROXY`、`ALL_PROXY` 及其他环境代理；不得仅依赖“配置中没有代理字段”或 `NO_PROXY`。
- 每个 POST 使用独立 cpr Session；GET 监听使用独立可取消执行路径，禁止多个线程共享同一个 cpr Session。
- 后台 Listener GET 与前台 POST SSE 恢复 GET 是两条独立流：两者可同时存在，分别拥有 cpr Session、事件 ID、`retry`、连续失败计数与停止信号；禁止为恢复前台结果而关闭或复用 Listener GET。
- 前台 POST、Server 请求响应和取消通知使用彼此独立的 cpr Session；共享的会话 ID、协议版本和关闭状态只通过受保护的 SDK 自有状态读取。
- 长期 SSE 不累计完整响应正文；网络回调只做增量解码和有界入队，不在 cpr 回调或传输锁内执行 Tool Handler。

#### 8.3.3 SSE 与恢复合同

新增 MCP 专用有状态 SSE 解码器，不能复用 DeepSeek `SSEParser`。它必须：

- 跨任意网络分块处理 LF、CRLF、注释行、未知字段、多行 `data:`、`id:` 和 `retry:`。
- 将 `id` 加空 `data` 的预热事件只用于更新当前流游标，不尝试解析 JSON。
- 把每个非空事件的 `data` 解析为一条 JSON-RPC 消息；在匹配请求响应之前允许出现 Server 请求和通知。
- 按流保存最后事件 ID，禁止把 POST 流和 GET 监听流的游标混用。
- POST SSE 提供事件 ID 后发生断线时，通过 GET + 当前流的 `Last-Event-ID` 恢复；遵守合法 `retry`，同时受连续重连次数和该前台请求绝对超时约束。
- POST SSE 恢复只续接流，绝不重新 POST 原始请求。没有事件 ID、恢复失败或绝对超时导致结果不可确认时，已 `Submitted` 且尚无终局响应的 `tools/call` 返回对应根因的 `OutcomeUnknown`；`ping()`、`listTools()` 等非工具操作保留 `RequestTimeout`、`TransportFailure` 或其他原始闭合错误码，不得泛化为 `OutcomeUnknown`。
- 后台 GET SSE 不受前台绝对请求超时限制。断线后按该流自己的事件 ID、`retry` 和连续重连上限恢复；成功建流并收到合法事件后重置连续失败计数。
- 后台 GET 意外断线后，有事件 ID 时使用 `Last-Event-ID` 续接；没有事件 ID 时只能在同一重连上限内发起新 GET，不伪造续传语义。
- 若 Server 声明 `tools.listChanged: true`，任何已进入 `Listening` 的 GET 意外出现空档时，Client 立即递增目录代次并标记 stale，因为空档期通知可能已丢失。恢复成功只恢复 Listener，不自动恢复旧 Catalog 或 Binding；上层必须重新列举和绑定。
- Server 声明 `tools.listChanged: true` 时，Listener 处于 `Recovering` 或 `Unavailable` 都属于监听不安全状态；`listTools()` 与 `callTool()` 必须在网络 I/O 前以 `ToolCatalogStale` 拒绝。只有 Listener 重新进入 `Listening` 后才允许发起新的 `listTools()`，且只有该次列举完整成功才能解除 stale；之前所有工具调用仍被拒绝。
- 后台 GET 没有“当前工具调用”，因此恢复耗尽时不返回 `OutcomeUnknown`。Listener 进入 `Unavailable`，并通过 `listenerState()` / `lastListenerFailureCode()` 暴露终态和脱敏原因，Client 保持 Ready；未声明 `tools.listChanged` 时 POST JSON/SSE 继续可用，声明该能力时 `listTools()` 与 `callTool()` 均以 `ToolCatalogStale` 拒绝且不发送请求，上层必须关闭并创建新 Client。首版不提供无限后台重试或单独重启 Listener 的公开 API。
- 初始 GET 返回 `405` 是 Server 明确不支持 Listener，对应 `Unsupported` 而非意外监听空档，不因 `tools.listChanged` 能力强制标记 stale；POST 响应流中收到的目录变化通知仍按正常合同处理。

已进入 Ready 后，后台 Listener 的每次建流或恢复结果按下表闭合处理；后台线程不向任意调用方抛异步异常：

| 结果 | 是否继续有限恢复 | Listener / Client 终态 | 可观测原因 |
|---|---|---|---|
| 建连失败、响应头超时、SSE 空闲超时或 HTTP `5xx` | 是；每次都重新获取凭据，共享该流的连续重连上限 | 尝试期间 Recovering；耗尽后 Unavailable + Ready | `lastListenerFailureCode()` 分别为 `TransportFailure`、`RequestTimeout` 或 `HttpStatusError` |
| 凭据 Provider 失败或超时 | 否；SDK 不自动刷新凭据 | Unavailable + Ready | `CredentialUnavailable` |
| HTTP `401` / `403` | 否 | Unavailable + Ready | `AuthenticationRequired` |
| 带会话 ID 的 HTTP `404` | 否 | Stopped + Faulted；按会话失效合同终止其他流 | `SessionExpired` |
| HTTP `405` | 否；不探测旧传输 | Unsupported + Ready | Listener 状态本身表示降级；若此前已经 Listening，仍按监听空档标记目录 stale |
| 其他 `3xx` / `4xx` | 否 | Unavailable + Ready | `HttpStatusError`；不跟随重定向 |
| `2xx` 但非 `text/event-stream`、非法 SSE 或非法 JSON-RPC | 否 | Stopped + Faulted | `ProtocolViolation` |
| `200 + text/event-stream` | 不适用 | Listening + Ready；收到首个合法事件后重置连续失败计数 | 清空非终命的 `lastListenerFailureCode()`；目录 stale 仍需新 `listTools()` 才能解除 |

`lastListenerFailureCode()` 返回可选的脱敏错误码，可与 `listenerState()` 并发读取且不占前台槽。Listener 处于 Recovering 时它表示最近一次失败，进入 Unavailable 后保留终止恢复的原因；成功回到 Listening 时清空。应用通过该读取 API 和 Listener 状态观察后台结果，首版不增加可阻塞内部线程的 Listener 用户回调。

#### 8.3.4 HTTP 会话失效

- 一旦初始化响应提供并通过校验的会话 ID，任何后续 POST 或 GET 收到 `404` 都表示整个会话失效；这包括 Initializing 阶段的 `notifications/initialized` POST、首次 Listener GET，以及 Ready 后的前台、控制和后台请求。
- Initializing 阶段发生上述 `404` 时，`connect()` 以 `SessionExpired` 失败，Client 进入 Faulted且不得发布 Ready。Ready 状态发生时，Client 进入 Faulted并停止所有 SSE；未提交的前台操作和非工具操作以 `SessionExpired` 结束，已提交但未收到终局响应的 `tools/call` 以带 `SessionExpired` 根因的 `OutcomeUnknown` 结束。任何可能产生副作用的请求都不得自动重放。
- Closing 状态下 DELETE 收到 `404` 只表示会话已不存在，按成功清理处理，不再次产生 `SessionExpired`。
- 旧 Client 清除会话、停止 SSE、完成本地清理并进入不可继续调用的状态，不在内部自动重新初始化。
- Agent 或应用创建新 Client，重新初始化、`listTools()`、筛选并绑定工具，避免旧 Binding 在目录可能变化后悄悄指向新会话。

#### 8.3.5 HTTP 凭据注入

首版采用外部凭据注入，不宣称完整实现 MCP OAuth：

- 凭据配置是闭合变体：`Anonymous`、`Bearer(provider)`、`FixedHeader(name, provider)`。`Anonymous` 不调用 Provider，其他两种必须提供 `shared_ptr<IMCPHttpCredentialProvider>`。
- `Bearer` 固定使用 `Authorization: Bearer <value>`；`FixedHeader` 的头名在构造配置时由受信应用固定并校验，Provider 每次只返回秘密值，不能动态选择头名。三种模式互斥。
- Provider 在每次前台 POST、Listener GET、任意恢复 GET、Server 请求响应 POST、取消通知 POST 和 DELETE 之前调用并返回当前秘密值；SDK 只为当前请求持有必要副本，完成后立即释放。
- 同一 `MCPClient` 通过一个独立凭据门串行调用 Provider，因此一个 Client 不会并发进入同一 Provider。若应用把同一 Provider 实例共享给多个 Client，Provider 自身必须保证跨 Client 线程安全；否则应为每个 Client 提供独立实例。
- Provider 请求上下文包含脱敏的请求种类、单调截止时间和协作式取消标志。Provider 必须在 `credential_timeout` 内返回，观察到取消后尽快退出，禁止同步递归调用同一 Client；这是接口合同，非协作、永不返回的用户实现不在 SDK 可强制终止的 C++ 能力内，不得用于验收。
- SDK 不在 Client 状态锁、Transport 锁或 cpr 回调内调用 Provider。等待凭据门、运行 Provider 和请求自身都受该前台操作绝对上限或后台状态机限制。
- HTTP 消息严格遵循 `prepareMessage()` / `commitPrepared()` 两阶段：Provider 只在准备阶段运行；准备成功后 Client 重新取得状态锁，按消息类型复核 Client、会话、Transport、请求、流或 Catalog 代次，再原子提交。提交阶段不得再次调用 Provider，物理网络发送发生在提交之后。
- Provider 由应用实现，并负责登录交互、Token 获取、安全存储、过期判断、刷新和注销。
- SDK 不解析 `WWW-Authenticate` 来启动 OAuth，不抓取 Protected Resource Metadata，不打开浏览器，不监听回调端口，也不自动扩大 Scope。
- Provider 自身抛出异常、达到 `credential_timeout` 或返回非法头时，根因为 `CredentialUnavailable`；若更早到达前台操作绝对上限，根因为 `RequestTimeout`；若 `close()` 先切换 Closing，根因为 `OperationCancelled`。这些都发生在该次准备请求提交前且不产生 MCP 网络 I/O，Provider 异常文本不进入公开错误。
- 上述根因的顶层表达取决于原始工具请求，而不是恢复请求自身：原始 `tools/call` 尚未提交时保留根因；原始调用已经 `Submitted`、恢复 GET 又在准备凭据时失败且仍无终局响应时，必须提升为 `OutcomeUnknown(cause=<对应根因>)`。后台 Listener 则按 Listener 矩阵记录根因。
- HTTP `401` / `403` 的脱敏根因统一为 `AuthenticationRequired`；非工具操作以它作为顶层错误，已提交且无终局响应的工具调用以它作为 `OutcomeUnknown` 根因。SDK 不自动刷新、重放或升级权限；应用可更新凭据后由业务人员根据幂等性明确决定是否发起新调用，SDK 不代为判断。
- 凭据只能放在请求头，禁止进入 URL；Header 名称和值拒绝 CR/LF。
- `FixedHeader` 不得使用 `Authorization`、MCP 保留头、`Host`、`Origin`、Cookie、代理头或 HTTP framing / hop-by-hop 头；头名和值都拒绝 CR/LF。
- `close()` 切换 Closing 后取消尚未开始的凭据请求，并向正在运行的 Provider 发出取消。DELETE 只在能于 `close_timeout` 剩余时间内取得凭据时尝试；凭据门忙、Provider 失败或剩余时间不足时跳过远端 DELETE，仍必须完成本地取消和资源回收，不得突破关闭总上限。

#### 8.3.6 HTTP 控制 POST 失败合同

HTTP 控制 POST 没有等待它的公开调用方，必须按用途显式承接错误；不得在线程入口吞掉异常：

| 控制消息 | 成功条件 | 失败后的主状态与 Listener | 错误去向 |
|---|---|---|---|
| `notifications/initialized` | `202`；兼容 `204` | 任一准备失败、提交失败、`401/403`、非成功状态、传输失败或超时都使 `connect()` 失败，Client 进入 Faulted，Listener 不得进入 Listening/Ready | 已有会话 ID 时 `404` 明确为 `SessionExpired`，其他失败由 `connect()` 抛出对应闭合根因；该消息不是工具调用，不使用 `OutcomeUnknown` |
| Server 请求响应，包括 `ping` 与 `-32601` | `202`；兼容 `204` | 凭据失败、提交失败、`401/403`、非成功状态、传输失败或控制响应上限到期，均表示无法继续履行协议：Client 进入 Faulted，Listener 进入 Stopped，并停止其他 SSE | `lastFailureCode()` 保存脱敏根因；当时已 `Submitted` 且无终局响应的工具调用以该根因为 `OutcomeUnknown`，其他等待操作返回原始根因 |
| `notifications/cancelled` | `202`；兼容 `204` | 除会话 `404` 外均为尽力失败，不改变 Client/Listener 终态，不覆盖原公开操作已经选定的错误；`404` 仍按会话失效进入 Faulted + Stopped | 非 `404` 只进入有界、脱敏内部诊断；`404` 写入 `lastFailureCode(SessionExpired)`，但原公开操作仍保留先发生的 `RequestTimeout` / `OperationCancelled` 等根因 |

所有控制 POST 都使用两阶段准备/提交和独立 cpr Session。Server 请求响应的控制上限取 `request_timeout` 与当前 Client/关闭剩余期限的较小值；失败后禁止自动重发该响应。DELETE 继续只按关闭合同处理，不属于本表。

控制 POST 失败与 `close()` 的线性化点统一由 Client 状态锁决定：只有失败先在 Connecting / Initializing / Ready 状态提交时，才能写入 Faulted 与 `lastFailureCode()`；若 `close()` 已先原子切换为 Closing，随后出现的 Provider 取消、提交取消、HTTP 取消、迟到状态或会话 `404` 都只是关闭收敛事件，不得再把状态改回 Faulted，也不得写入或覆盖终命 `lastFailureCode()`，最终必须进入 Closed。若真实控制失败先进入 Faulted，随后显式 `close()` 仍按 Faulted → Closing → Closed 清理，并保留原终命失败码供只读诊断。

### 8.4 Client 生命周期与状态

公开状态使用闭合枚举：

```text
Disconnected -> Connecting -> Initializing -> Ready -> Closing -> Closed
                    \          |          /
                     --------> Faulted -> Closing -> Closed
```

状态合同：

- 构造完成后为 `Disconnected`，构造期间不执行 I/O。
- `connect()` 仅允许从 `Disconnected` 进入；它启动进程、发送 `initialize`、校验版本和能力、发送 `notifications/initialized`，成功后进入 `Ready`。
- `listTools()`、`callTool()` 和 `ping()` 仅允许在 `Ready` 使用。
- `close()` 对 `Disconnected`、`Closed` 和重复调用保持幂等；从 Ready 或 Faulted 调用都会先进入 Closing，完成有界清理后进入 Closed。
- 协议违规、Server 异常退出或不可恢复的传输失败进入 `Faulted`。
- `Closed` 或 `Faulted` 不允许再次 `connect()`，避免复用残留请求 ID、线程或进程状态。
- `close()` 并发终止 `connect()`、`listTools()` 或用户 `ping()` 时，在途操作以 `OperationCancelled` 结束；终止未提交的 `callTool()` 同样返回 `OperationCancelled`，而已提交且没有终局响应的 `callTool()` 返回带 `OperationCancelled` 根因的 `OutcomeUnknown`。`initialize` 不发送 MCP 取消通知，其他已发送请求尽力发送取消通知。
- Client 保存只读、脱敏的 `lastFailureCode()`，它只表示使主状态进入 Faulted 的终命原因；非终命 Listener 错误单独由 `lastListenerFailureCode()` 表达。Faulted 经 `close()` 进入 Closed 后仍可读取最后失败类别，但不能读取原始协议正文或秘密。
- 析构函数 `noexcept`，执行有界的最佳努力清理，但示例和文档必须要求显式 `close()`。

HTTP Listener 使用独立闭合状态 `NotApplicable`、`Starting`、`Listening`、`Unsupported`、`Recovering`、`Unavailable`、`Stopped`。它不替代 Client 主状态：`405` 对应 Ready + Unsupported，运行期重连耗尽对应 Ready + Unavailable，会话 `404` 则使整个 Client 进入 Faulted。

前台与控制路径发生错误后的 Client 终态按下表唯一确定；`OutcomeUnknown` 必须携带表中实际终态：

| 根因或完成路径 | Client 终态 | 说明 |
|---|---|---|
| 配置/参数错误、`ClientBusy`、提交前 `ToolCatalogStale`、提交前 Provider 失败 | 保持进入操作前的状态，通常为 Ready | 准备请求未提交，零 MCP 网络写入 |
| `connect()` 任一初始化或 `notifications/initialized` 失败 | Faulted | 连接从未对外发布为 Ready |
| Ready 下单次前台 HTTP `401/403`、非 `404` 的 `3xx/4xx/5xx` | Ready | 当前 POST 失败但会话与独立 Listener 未被证明失效；Submitted 工具仍为 `OutcomeUnknown` |
| 前台请求/绝对超时、单次 HTTP POST 网络失败、POST SSE 无事件 ID或恢复耗尽 | Ready | 取消/恢复仅影响当前操作；若并发发生更高优先级终命事件，则采用对应 Faulted/Closed 终态 |
| JSON-RPC `error` 或合法工具 `isError: true` | Ready | 已收到终局结果，结果确定 |
| 运行期 Listener 恢复耗尽、凭据失败、`401/403`、`405` 或非终命 HTTP 错误 | Ready | Listener 按独立矩阵进入 Unavailable/Unsupported，主 Client 仍可按能力合同工作 |
| stdio EOF、Server 退出、管道永久失败、协议违规、消息队列溢出、Server 请求响应控制 POST 失败 | Faulted | 连接或协议连续性已不可恢复 |
| 带会话 ID 的 HTTP `404` | Faulted | 整个会话失效，停止全部 SSE |
| 并发 `close()` 先取得状态线性化点 | Closed | 清理完成后是唯一终态；迟到的控制 POST 失败不得改回 Faulted，Submitted 工具返回 `OutcomeUnknown(cause=OperationCancelled)` |

### 8.5 JSON-RPC 与基础协议

| 编号 | 需求 |
|---|---|
| MCP-FR-020 | 使用 UTF-8 JSON-RPC 2.0；发送请求 ID 使用单调递增、会话内不复用的整数。 |
| MCP-FR-021 | 严格校验 `jsonrpc`、`id`、`result` / `error` 互斥关系，以及响应 ID 是否属于当前请求。 |
| MCP-FR-022 | 首版只支持协议版本 `2025-11-25`；Server 协商为其他版本时关闭连接并报告版本不兼容。 |
| MCP-FR-023 | Client 的 `initialize` 能力对象为空，不声明 Roots、Sampling、Elicitation 或 Tasks。 |
| MCP-FR-024 | 首版是 Tools 专用 Client；Server 初始化结果未声明 `tools` 能力时，`connect()` 抛出 `CapabilityMissing`、清理传输并进入 `Faulted`，不得进入 Ready。 |
| MCP-FR-025 | Client 必须响应 Server 发来的 `ping` 请求；未声明能力对应的其他 Server 请求返回 JSON-RPC `-32601`。 |
| MCP-FR-026 | 合法但未使用的 Server 通知不得破坏当前请求关联；`notifications/tools/list_changed` 不自动改写注册表。 |
| MCP-FR-027 | 普通请求超时后发送 `notifications/cancelled` 并停止等待；初始化请求超时不得发送取消通知。取消通知只是尽力请求，不能把已提交工具调用的结果从未知改为“确定未执行”。 |
| MCP-FR-028 | Server EOF 或退出时，普通等待操作以统一的连接关闭错误结束；已提交且未收到终局响应的工具调用以带 `ServerExited` 或 `TransportFailure` 根因的 `OutcomeUnknown` 结束。 |
| MCP-FR-029 | 收到 `notifications/tools/list_changed` 时递增工具目录代次、标记目录失效并发出有界通知；不得自动列举或修改 `ToolRegistry`。 |

### 8.6 Tools 列举

`MCPClient::listTools()` 返回只读 `MCPToolCatalog`，其中包含工具列表以及由 Client 签发的目录代次令牌，并且必须：

- 从空游标开始调用 `tools/list`，持续跟随非空 `nextCursor`。
- 保持 Server 返回顺序，不依赖哈希容器迭代顺序。
- 对重复游标、超过页数、超过工具数、非法工具对象、缺失名称和非对象 `inputSchema` 给出确定失败。
- 跨页出现同名远端工具时拒绝整批结果，不能只保留最后一个。
- 完整保留 `name`、`title`、`description`、`inputSchema`、`outputSchema`、`annotations`、`icons`、`execution` 及未知扩展字段。
- 不实现完整 JSON Schema 语义验证；只验证本任务需要的顶层类型与必填字段，避免新增 Schema 依赖。
- 成功返回后将当前代次标记为已解析；Catalog 只能由 `MCPClient` 构造并可安全复制，Adapter 同时校验 Client 身份与代次，调用方不能手工拼接有效令牌。

`MCPTool` 应同时提供常用字段和原始 JSON 对象，保证协议扩展不会因本地模型不完整而丢失。

目录变化合同：

- Client 维护单调递增的 `catalog_revision`；连接后未成功列举前目录不可用于绑定。
- 收到 `notifications/tools/list_changed` 后立即递增代次并标记 stale，旧 Catalog 和旧 Binding 不再有效。
- `listTools()` 开始时捕获目录代次；分页期间收到变化通知时，本次列举以 `ToolCatalogStale` 失败并丢弃全部临时页，禁止发布跨代次混合 Catalog。
- Server 声明 `tools.listChanged: true` 且 Listener 为 `Recovering` / `Unavailable` 时，不得开始或发布新 Catalog；恢复期间发起的 `listTools()` 必须在首个 POST 之前失败。Listener 在分页中由 `Listening` 变为 `Recovering` 时等价于目录代次变化，丢弃已收集页。
- `callTool(catalog, ...)` 必须验证 Catalog 由当前 Client 签发、包含目标远端工具且代次与当前目录一致。验证与“已提交到传输发送队列”的状态迁移必须在同一状态锁下原子完成，禁止先检查、解锁后再发送的 TOCTOU 窗口。
- 若变化通知或监听空档先于发送提交，调用以 `ToolCatalogStale` 失败且不产生网络写入；若请求先原子提交，后到通知只阻止后续调用，该请求继续等待并返回真实结果，不能假定远端尚未执行。
- 旧 Catalog 即使在其他调用方完成新一轮 `listTools()` 后也不会重新有效；上层必须使用新 Catalog 创建新 Binding。
- Client 提供只读 `catalogRevision()` / `isToolCatalogStale()`，并允许配置轻量目录变化回调；回调只传递 `server_id` 与新代次，不携带工具正文。
- 目录变化回调运行在协议分发上下文，只能发出信号或入队；其异常必须隔离，禁止在回调中同步递归调用同一 Client。
- SDK 不自动刷新、不保留上层白名单、不重设别名或风险，也不自动改写 `ToolRegistry`。

### 8.7 Tools 调用

`MCPClient::callTool(catalog, remote_name, arguments)` 必须：

- 只接受当前 Client 签发且未过期的 `MCPToolCatalog`；目标名称必须存在于该 Catalog，禁止用不携带目录代次的字符串调用重载绕过安全检查。
- 只接受 JSON 对象参数；调用方参数原样进入 `tools/call`。
- 返回独立的 `MCPToolCallResult`，至少包含 `content`、可选 `structuredContent`、`isError`、可选 `_meta` 和原始结果对象。
- 保持 JSON-RPC 协议错误与 `isError: true` 工具业务失败的区别。
- 不自动重试。工具可能产生外部副作用，重试策略只能由知道幂等性的上层决定。
- 不把 Server stderr、启动命令、环境变量或原始传输错误拼进模型可见结果。

`MCPToolCallResult` 是 MCP Client 的无损结果合同；现有 `ToolResult` 只是 Adapter 层的有损兼容视图。首版不得为了 MCP 偷改核心 `ToolResult` 语义。

工具调用结果确定性合同：

- 唯一“提交点”是在 Client 状态锁下完成 Catalog / 状态校验，并把完整 JSON-RPC 请求原子提交到 Transport 发送队列的瞬间。在此之前失败为 `NotSubmitted`，保留原始错误码且可断言零网络写入；从此刻起为 `Submitted`，即使 Transport 尚未报告物理写入也按保守未知处理。
- 只有收到与该请求 ID 匹配、并通过校验的 JSON-RPC `result` 或 `error` 才是终局响应。`result` 中的 `isError: true` 与 JSON-RPC `error` 都属于“结果已知”；HTTP 状态、取消通知、连接关闭或本地超时都不是工具终局响应。
- `Submitted` 后到终局响应前发生请求/绝对超时、HTTP 非成功状态、EOF / Server 退出、传输失败、会话失效、队列溢出、无法关联终局结果的协议违规或并发 `close()`，均返回 `MCPException(OutcomeUnknown)`。异常必须携带脱敏 `causeCode`、`mayHaveExecuted=true` 与实际 Client 终态，不得把根因直接替换成可被上层当作普通可重试错误的顶层码。
- Adapter 将 `OutcomeUnknown` 映射为稳定中文失败，必须明确“工具可能已执行，请勿自动重试”，只可附带脱敏根因类别；SDK、Adapter 和示例均不自动重放。

### 8.8 Tool Adapter

新增无隐式注册副作用的 `MCPToolAdapter`：

1. 输入 `shared_ptr<MCPClient>`、该 Client 返回的 `MCPToolCatalog`、调用方选定的工具集合和 Adapter 选项。
2. 先对整批工具完成名称、别名、Schema、重复项和目标注册表冲突预检。
3. 输出 `MCPToolBinding` 列表；每个绑定包含现有 `Tool`、`ToolHandler`、远端原名和 Server 标识。
4. 由调用方审计并显式写入 `ToolRegistry`；可提供便利注册函数，但必须在写入前完成全部可预见校验，禁止校验到一半才发现冲突。

命名合同：

- 默认本地名为 `<server_id>__<remote_name>`，避免不同 Server 与本地工具直接重名。
- 本地名必须满足首版可移植合同 `[A-Za-z0-9_-]{1,64}`。
- 默认映射不做破坏性的自动替换或截断；不满足合同时，调用方必须提供显式别名。
- 显式别名、默认名以及别名之间都必须做冲突检测；冲突默认报错，不静默覆盖。
- Adapter 必须保存“本地名 -> 远端原名”的确定映射，实际 `tools/call` 始终使用远端原名。

风险合同：

- 所有 MCP 工具默认映射为 `ToolRiskLevel::High`。
- Server 的 `annotations` 只能作为不受信任的参考元数据，不能自动降低风险。
- 应用可以逐项显式覆盖本地风险等级；审批与阻断仍由上层负责。

所有权合同：

- `MCPToolBinding` 的 Handler 捕获 `weak_ptr<MCPClient>`，不得因为工具已注册就让 Client 和 Server 子进程永久存活。
- Binding 同时捕获由 Client 签发的 Catalog 不透明状态，Handler 必须调用携带该 Catalog 的同一安全调用路径，不得退化为无代次的 `callTool(remote_name, arguments)`。Client 不存在、未 Ready、已关闭、Faulted、监听安全条件不满足或目录代次不同时，Handler 返回稳定的中文失败结果，不发送远端调用。
- 上层收到目录变化信号后，应先停止把旧 Binding 的 Tool 定义放入新的 `ChatRequest`，再批量注销旧绑定、重新列举、筛选并注册。

`ToolRegistry` 通用增强：

- 新增不依赖 MCP 类型的 `unregisterTool(name)` 和 `unregisterTools(names)`。
- 批量注销先校验空名称和重复名称，再统一修改定义、Handler 与展示顺序；未知名称按幂等清理处理，不形成部分失败。
- 注销后再次注册同名工具视为新的注册位置，保持确定顺序。
- 注册表仍不提供内部线程同步；上层不得在工具执行或列举并发期间修改注册表。
- Adapter 可提供 `unregisterBindings(registry, bindings)` 便利函数，但调用动作必须由上层显式触发。

### 8.9 MCP 结果到 ToolResult 的映射

| MCP 结果 | Adapter 行为 |
|---|---|
| `isError` 为 `false`，包含文本和/或对象 `structuredContent` | 返回成功 `ToolResult`；`data` 保存标准化的 MCP 结果信封，不丢失文本与结构化字段。 |
| `isError` 为 `true` | 提取有界文本生成失败 `ToolResult`；由于现有失败工厂不保存 `data`，结构化失败正文只在 `MCPToolCallResult` 中无损存在。 |
| JSON-RPC `error` | Client 抛出可分类 MCP 异常；Adapter 转为脱敏后的失败 `ToolResult`。 |
| `OutcomeUnknown` | 返回固定中文失败“MCP 工具结果未知，工具可能已执行，请勿自动重试”；可追加脱敏 `causeCode`，不得降级成普通可重试连接失败。 |
| 图片、音频、资源链接或内嵌资源 | Client 原样保留；Adapter 返回明确的“不支持 MCP 富媒体工具结果”失败，不把 Base64 或资源正文直接塞给模型。 |
| Client 弱引用失效或连接不可用 | 返回“该 MCP 连接不可用”的稳定失败，不触发进程重启。 |

错误文本必须有长度上限，并过滤控制字符。首版不尝试从富媒体或任意资源 URI 自动读取更多数据。

## 9. 建议公开接口轮廓

以下只固定职责和语义，具体签名允许在实现评审时按仓库风格微调，但不得改变本 PRD 的边界。

```text
namespace aiSDK {

struct MCPServerConfig;
struct MCPStdioServerConfig;
struct MCPStreamableHttpConfig;
struct MCPTool;
class MCPToolCatalog;
struct MCPToolCallResult;
struct MCPToolBinding;
struct MCPToolAdapterOptions;
struct MCPHttpCredentialRequestContext;

class IMCPHttpCredentialProvider {
    credentialValue(request_context); // 返回当前秘密值
};

enum class MCPClientState;
enum class MCPListenerState;
enum class MCPErrorCode;

class MCPException : public std::runtime_error {
    code() const;
    causeCode() const;
    mayHaveExecuted() const;
    clientStateAtFailure() const;
};

class IMCPTransport {
    open(message_callback, transport_event_callback);
    prepareMessage(message, request_context); // 锁外准备，无 MCP I/O
    commitPrepared(prepared_message);         // 状态锁内原子入队
    completeInitialization(protocol_version);
    close() noexcept;
};

class MCPClient {
    explicit MCPClient(config, optional_transport);
    connect();
    state() const;
    listenerState() const;
    lastFailureCode() const;
    lastListenerFailureCode() const;
    ping();
    listTools(); // 返回 MCPToolCatalog
    catalogRevision() const;
    isToolCatalogStale() const;
    callTool(catalog, remote_name, arguments);
    close() noexcept;
};

class MCPToolAdapter {
    adaptTools(client, catalog, selected_tools, options, optional_registry_snapshot);
    registerBindings(registry, bindings);
};

}
```

公开头文件可使用项目已经接受的 `nlohmann::json` 作为 JSON 值合同，但不得暴露 Win32、POSIX、cpr、libcurl、线程、管道或社区 MCP SDK 类型。HTTP 后端测试接口和 SSE 解码器均为 `ai_sdk_mcp` 内部合同。

## 10. 内部并发与 I/O 模型

首版把“前台公开操作准入”与“Transport 后台任务”明确分离：

- **前台公开操作槽**：每个 `MCPClient` 只允许一个由调用方发起的 `connect()`、`ping()`、`listTools()` 或 `callTool()` 在途；`listTools()` 的多页请求占用同一个槽直到完成。
- **后台 Transport 任务**：长期 GET SSE、GET 重连、stdio stdout/stderr 读取、消息解析、Server 请求与通知、Server `ping` 响应、取消通知和 HTTP 控制 POST 都不占前台公开操作槽。
- GET SSE 建立后，`callTool()` 仍能正常取得前台槽；只有另一个调用方发起的公开操作尚未结束时才返回 `ClientBusy`。
- 其他并发公开操作立即返回 `ClientBusy`，不在内部排队，也不等待前一个工具无限结束。
- `state()` 可安全并发读取；`close()` 是唯一允许并发发起的控制操作，它进入 Closing、取消传输并让在途操作确定结束。
- `listenerState()`、`lastFailureCode()` 与 `lastListenerFailureCode()` 同样可以安全并发读取，不占前台操作槽。
- 需要并行工具调用时，由 Agent 或应用创建多个独立 `MCPClient`，或在上层实现 Client Pool；首版 `ai_sdk_mcp` 不提供连接池、负载均衡或会话租借 API。
- 同一 Server 的多个 Client 具有独立进程或 HTTP 会话，Pool 不得假设它们共享 Server 状态，也不得在失败后自动重放可能产生副作用的工具调用。
- `ToolRegistry` 仍不提供并发保证；并发注册、删除或执行的互斥责任不因 MCPClient 的保护而改变。
- `connect()` 保持前台槽，直到 MCP 初始化完成且首次 GET 响应头被分类：`200 + text/event-stream` 将已建立流移交后台并进入 Ready + Listening，`405` 停止 GET 并进入 Ready + Unsupported，其他结果使连接失败。它不等待 `200` 流结束；只有 Ready 之后的断线、有限恢复和目录过期处理属于 Transport 后台任务。
- `stdio` 内部使用一个 stdout 读取线程；HTTP 最多同时存在一条后台 Listener GET SSE、一条当前前台 POST SSE 的恢复 GET、当前前台操作的一条 POST JSON/SSE，以及必要的短生命周期控制 POST。
- “一个公开操作在途”不等于底层只有一个 HTTP 连接：在等待 POST SSE 响应期间，Client 仍需通过独立 POST 发送取消通知或响应 Server 请求。
- 所有需要分发的请求、响应和通知先进入公共有界队列，再由协议层关联；网络回调和传输锁内不得调用 Tool Handler。
- 队列满时，请求、响应、`ping` 等不可丢消息触发 `MessageQueueOverflow` 并使 Client 进入 Faulted。重复 `tools/list_changed` 通过原子 stale 标记合并，SSE 注释/保活在传输层消费，合法未知通知可在入队前忽略；不得静默丢弃可能影响请求结果的消息。
- stderr 使用独立消费路径，防止管道写满；默认丢弃，只保留有界诊断尾部或交给调用方显式配置的非阻塞回调。HTTP SSE 不累计完整响应正文。
- stderr 回调运行在内部消费线程，回调异常必须被隔离，不能改变协议或业务结果。
- `close()`、EOF、HTTP 取消、SSE 重连、超时和析构与后台线程之间必须有明确停止信号和有限等待，禁止永久 `join()`。

选择单前台操作的理由：当前 `ToolHandler` 同步、`ToolExecutor` 串行、注册表无并发保证。首版引入多个调用方请求并发只会扩大关闭竞态、乱序关联与测试范围；后台 Transport 控制流则是协议正确运行所必需，不受这一限制。

## 11. 错误合同

新增闭合的 `MCPErrorCode`，至少区分：

- `VersionMismatch`
- `CapabilityMissing`
- `ProtocolViolation`
- `RemoteProtocolError`
- `TransportFailure`
- `RequestTimeout`
- `ServerExited`
- `HttpStatusError`
- `AuthenticationRequired`
- `CredentialUnavailable`
- `ClientBusy`
- `OperationCancelled`
- `ToolCatalogStale`
- `SessionExpired`
- `OutcomeUnknown`
- `MessageLimitExceeded`
- `MessageQueueOverflow`
- `PaginationLimitExceeded`

`MCPException` 除顶层 `code()` 外，还提供只读 `causeCode()`、`mayHaveExecuted()` 与 `clientStateAtFailure()`。只有顶层码为 `OutcomeUnknown` 时 `mayHaveExecuted()` 为 `true` 且 `causeCode()` 必须有值；其 `clientStateAtFailure()` 保存错误完成线性化点上的真实 Client 状态，之后调用方再执行 `close()` 不会回写该快照。其他错误的布尔值为 `false`，状态快照仍可用于确定性诊断。根因与状态只使用闭合枚举，不携带原始网络正文或秘密。

异常与结果矩阵：

| 场景 | SDK 表达 |
|---|---|
| 配置字段非法、Adapter 别名或 Schema 非法 | `std::invalid_argument` |
| 在错误生命周期调用公开方法 | `std::logic_error` |
| 协议、传输、HTTP 状态、版本、能力、超时、Server 退出 | 非工具操作或 NotSubmitted 工具调用抛出派生自 `std::runtime_error` 的 `MCPException`，携带原始闭合错误码；Submitted 工具调用按本表 `OutcomeUnknown` 规则提升顶层码 |
| HTTP `401` / `403` | 非工具操作返回 `MCPException(AuthenticationRequired)`；Submitted 工具调用无终局 JSON-RPC 响应时返回 `OutcomeUnknown(cause=AuthenticationRequired)`；两者都不得自动重放 |
| 凭据 Provider 失败、超时、取消或返回非法请求头 | Provider 自身失败为 `CredentialUnavailable`，前台绝对上限先到为 `RequestTimeout`，`close()` 先发生为 `OperationCancelled`；原始工具请求已 Submitted 且无终局响应时，统一提升为 `OutcomeUnknown(cause=<对应根因>)` |
| 已有公开操作在途时发起另一个公开操作 | `MCPException(ClientBusy)`；不排队、不发送网络消息 |
| `close()` 终止尚未完成的公开操作 | 普通操作和 NotSubmitted 工具调用返回 `MCPException(OperationCancelled)`；Submitted 且无终局响应的工具调用返回 `OutcomeUnknown(cause=OperationCancelled)`；Client 最终进入 Closed |
| 工具目录变化后使用旧 Catalog、旧 Binding 或直接调用工具 | `MCPException(ToolCatalogStale)` 或 Adapter 失败 `ToolResult`；不得发送远端请求 |
| 已有会话 ID 的后续 POST/GET 返回 `404` | Initializing 阶段使 `connect()` 返回 `SessionExpired`；Ready 下普通操作或 NotSubmitted 调用返回 `SessionExpired`，Submitted 且无终局响应的工具调用返回 `OutcomeUnknown(cause=SessionExpired)`。Client 进入 Faulted，由上层创建新 Client，不重放当前操作 |
| Closing 状态 DELETE 会话 `404` | 按会话已不存在处理，完成清理并进入 Closed |
| Submitted 工具调用在终局响应前失去确定性 | `MCPException(OutcomeUnknown)`，携带实际根因（如 `RequestTimeout`、`TransportFailure`、`ServerExited`、`SessionExpired`、`OperationCancelled`）与 `mayHaveExecuted=true`；明确提示工具可能已经执行 |
| 后台 GET 意外断线且 Server 声明 `tools.listChanged` | Listener 为 Recovering 或 Unavailable 时目录保持 stale，`listTools()` / `callTool()` 以 `ToolCatalogStale` 拒绝且零网络写入；只有恢复 Listening 后的完整新列举可解除 stale。恢复耗尽时 Client 保持 Ready，上层创建新 Client |
| 后台 GET 恢复次数耗尽且 Server 未声明 `tools.listChanged` | Listener 进入 Unavailable，`lastListenerFailureCode()` 保留终止原因；Client 保持 Ready，POST 能力继续可用 |
| 协议队列无法接收不可丢消息 | Client 进入 Faulted；普通操作返回 `MCPException(MessageQueueOverflow)`，Submitted 且无终局响应的工具调用返回 `OutcomeUnknown(cause=MessageQueueOverflow)` |
| Server 返回 JSON-RPC `error` | `MCPException(RemoteProtocolError)`；可保留数字远端错误码，但不暴露未经处理的敏感正文 |
| 合法 `tools/call` 结果中 `isError: true` | 正常返回 `MCPToolCallResult`，不抛异常 |
| Adapter 内的工具业务失败 | `ToolResult::errorResult(...)`，不打断同批其他工具 |
| Adapter 遇到图片、音频或资源内容块 | 返回固定中文 `ToolResult` 失败；该分类不属于 MCP Client 协议异常码 |
| `close()` 或析构清理失败 | `close()` 不抛；记录有界状态供诊断，析构绝不传播异常 |

所有 SDK 自行生成的错误提示使用简体中文。公开错误不得包含环境变量值、完整命令行、stderr 原文、Server 返回的大段数据或本机敏感路径；需要底层诊断时只保留错误类别、Server 非敏感标识和平台错误码。

## 12. 安全要求

### 12.1 进程与配置

- Server 配置是受信任的应用配置，不得由模型输出直接构造。
- 只接受“可执行文件路径 + 参数数组”，不通过 shell 解释，不拼接命令字符串。
- 默认不继承父进程环境；凭据只通过应用显式选择的环境项传入。
- executable、working directory 和环境变量在启动前校验；失败时不得留下部分创建的进程或管道。
- Windows 使用 Job Object、Linux 使用独立进程组，确保超时与析构能够回收整个受控进程树。

### 12.2 协议与资源

- 对单消息大小、工具页数、工具总数、错误文本和 stderr 缓存设置上限。
- stdout 只允许协议消息；任何日志必须走 stderr。
- 不执行 Server 返回的资源 URI，不自动解码或保存富媒体内容。
- 不把未知 JSON 字段解释为本地指令；未知字段只在原始结果中保存。
- 不自动重试可能产生副作用的工具调用。

### 12.3 HTTP Endpoint 与网络

- Endpoint 只能来自应用的受信配置，Client 构造后不可变；模型、Skill、MCP Tool 与 Server 响应都不能改变目标地址。
- 普通用户界面可以收集 Endpoint，但应用必须在构造 Client 前完成校验、授权和持久化；SDK 不提供运行期改址 API。
- 默认只允许 HTTPS；明文 HTTP 仅能通过显式开发选项访问严格的 loopback 地址。
- TLS 证书验证始终开启，首版不提供 `verify=false` 逃生开关。
- TLS 使用操作系统或 cpr/libcurl 平台后端的系统信任库；首版不读取生产自定义 CA Bundle，不加载客户端证书/私钥，也不配置 HTTP(S) 代理。
- 生产 HTTP 后端对每个 cpr/libcurl Session 显式设置空代理，禁止环境变量隐式改写路由；首版没有任何代理配置或代理凭据入口。
- 首版禁止自动 HTTP 重定向；任意 `3xx` 返回可分类错误，防止凭据、会话 ID 或恢复游标泄漏到其他 Origin。
- URL 拒绝用户信息、Fragment 和 CR/LF；凭据不得放在查询参数中。
- 原生 C++ 客户端不主动构造 `Origin`，也不实现浏览器 CORS 抽象。
- 自定义头与凭据 Provider 不得覆盖 `Accept`、`Content-Type`、`Host`、`Origin`、`MCP-Protocol-Version`、`MCP-Session-Id` 或 `Last-Event-ID`，且头名称和值必须拒绝 CR/LF。
- Endpoint 完整查询参数、Authorization、会话 ID、事件 ID、响应正文和动态鉴权数据不得进入日志、Trace 或公开异常。
- SDK 不持久化 Token，不自动刷新 Token，不因 `401/403` 重放请求；凭据生命周期全部属于应用。

### 12.4 工具暴露

- Adapter 默认将远端工具标记为高风险。
- 调用方必须显式选择工具并注册；SDK 不自动“全量同步”。
- 名称冲突、别名冲突和 Schema 顶层类型不合法时，整批适配失败。
- Server 注解、描述和工具结果均按不受信任内容处理，不能改变应用权限。

## 13. Trace 与日志

首版 Trace 方案：

- MCP 工具经现有 `ToolExecutor` 执行时，继续使用现有 `ToolExecution` 步骤，记录适配后的本地工具名、成功状态和既有白名单字段。
- 不新增 MCP 协议子步骤，不在 `ToolHandler` 内寻找或伪造父 Trace ID。
- 不使用全局变量或线程局部变量传递 Trace 上下文。
- Server 命令、参数、环境、HTTP Endpoint 查询参数、鉴权头、会话 ID、事件 ID、原始请求、原始响应、stderr 和富媒体内容不得进入 Trace。

若未来需要 MCP 连接、分页或远端调用子步骤，应先单独设计显式 `ToolExecutionContext`，再扩展闭合的 Trace 枚举与白名单；不得在本任务中越过现有 Trace 合同。

日志方案：

- SDK 不默认把 Server stderr 转发给 `spdlog`。
- 调用方显式配置 stderr 回调时，SDK 只负责分块和上限，不承诺内容安全；文档必须提示调用方自行脱敏。
- 内部错误日志仅记录闭合错误类别、Server 非敏感标识、HTTP 状态码和必要计数，不记录协议正文与秘密。

## 14. 性能与资源预算

- JSON 解析时间复杂度应与单条消息大小线性相关，即 `O(n)`。
- `tools/list` 的内存占用与最终工具数量和 Schema 总大小线性相关，并受页数、工具数和消息大小三重上限约束。
- 单连接固定使用有限后台线程；不得按请求或按分页创建新线程。
- stderr 诊断采用有界环形尾部缓存或直接丢弃，不能无限累积。
- SSE 解码缓冲、单事件、消息队列和重连次数都有硬上限；长期 GET 不保留完整流正文。
- 同步 Tool Handler 会阻塞调用线程直到结果或超时；示例与 API 文档必须明确该行为。
- 首版不以吞吐量为目标；正确关闭、资源有界和可诊断错误优先于多请求并发。

## 15. 文件与目录规划

建议落点如下，最终实现可在不改变分层的前提下微调文件粒度：

```text
include/mcp/
  MCPServerConfig.h
  MCPTypes.h
  IMCPTransport.h
  MCPClient.h
  MCPToolAdapter.h

src/mcp/
  MCPClient.cpp
  MCPProtocol.cpp
  MCPToolAdapter.cpp
  StdioMCPTransport.cpp
  StreamableHttpMCPTransport.cpp
  detail/
    MCPProtocol.h
    MCPHttpBackend.h
    MCPSseDecoder.h
    Process.h
  process/
    WindowsProcess.cpp
    PosixProcess.cpp

现有核心文件增量：
  include/tool/ToolRegistry.h
  src/tool/ToolRegistry.cpp
  tests/tool/tool_registry_test.cpp

tests/mcp/
  CMakeLists.txt
  mcp_protocol_test.cpp
  mcp_client_test.cpp
  mcp_tool_adapter_test.cpp
  mcp_sse_decoder_test.cpp
  mcp_streamable_http_test.cpp
  mcp_stdio_integration_test.cpp
  mcp_http_integration_test.cpp
  fixtures/
    mcp_test_server.cpp
    mcp_http_test_server.cpp
    mcp_tls_test_server.cpp
    tls/
      WindowsTlsEndpoint.cpp
      PosixTlsEndpoint.cpp
      test_root_ca.pem
      test_server_cert.pem
      test_server_key.pem
      test_server.pfx
      self_signed_cert.pem
      self_signed_key.pem
      self_signed_server.pfx

examples/08_mcp_tool_call/
  CMakeLists.txt
  main.cpp
```

内部协议编解码器和平台进程类型保留在 `src/mcp/detail/`，不得为了测试提升为公开 API。当前示例编号 `07` 已由 Trace 使用，因此 MCP 示例使用 `08_mcp_tool_call`。

## 16. 测试策略

### 16.1 测试原则

- 所有必测用例本地运行、无公网、无 API Key、无固定端口、无人工 Server。
- 协议和 Client 测试注入脚本 `IMCPTransport`；HTTP 传输测试注入内部脚本 `IMCPHttpBackend`；真实进程与真实 cpr 行为分别由仓库内编译的 C++ 子进程夹具和回环 HTTP Server 验证。
- 测试 Server 位置通过 `$<TARGET_FILE:...>` 传入，不拼接 `.exe`，不依赖当前工作目录；HTTP Server 绑定 loopback 动态端口，并通过确定的就绪信号返回实际 Endpoint。
- 除明确的超时测试外，不使用任意 `sleep`；用脚本响应、条件变量、EOF 或进程状态同步。
- 协议和状态机单元测试注入可控时钟，不依赖真实墙钟；真实进程/HTTP 超时测试使用确定就绪信号和 2～5 秒操作截止时间，避免 Windows/WSL 负载造成 250 毫秒级抖动。
- 真实 `stdio` 与 HTTP 集成目标的 CTest 总超时默认 30 秒；超时必须由 CTest 判定失败，不允许测试进程永久等待。
- 所有 MCP 测试设置 CTest 标签 `mcp`；真实进程与 HTTP 目标再分别设置 `integration`、`stdio` 或 `http` 标签，支持 `ctest -L mcp` 精确筛选。

### 16.2 自动化测试矩阵

| 层级 | 必测内容 | 目标 |
|---|---|---|
| JSON-RPC | 请求、响应、通知、唯一 ID、错误 `jsonrpc`、`result/error` 冲突、非法 JSON、未知 ID、消息与队列上限、不可丢消息溢出进入 Faulted | `ai_sdk_mcp_test` |
| stdio 分帧 | 分块读取、多消息连续到达、LF、CRLF、空行、EOF 残缺尾部、stdout 噪声、stderr 噪声 | `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test` |
| SSE 解码 | 每个字节位置切分、LF/CRLF、跨块 CRLF、多行 data、id、空 data、retry、注释、未知字段、事件上限 | `ai_sdk_mcp_test` |
| 生命周期 | 无 I/O 构造、初始化顺序、版本协商、缺 Tools 时 connect 失败、initialized 通知、重复连接、close 取消 connect/list/call、Faulted 清理到 Closed、析构清理 | `ai_sdk_mcp_test` |
| `tools/list` | 单页、多页、空列表、重复游标、跨页重名、非法工具、页数与工具数上限、分页中 list_changed 丢弃混合 Catalog | `ai_sdk_mcp_test` |
| `tools/call` | Catalog 归属与工具成员校验、参数透传、文本、结构化内容、混合内容、`isError`、JSON-RPC 错误、错误 ID、超时、EOF、发送前/发送后 list_changed 竞态 | `ai_sdk_mcp_test` |
| Adapter | 默认命名、显式别名、非法名称、本地与多 Server 冲突、Schema、默认高风险、弱引用失效、结果映射 | `ai_sdk_mcp_test` |
| 目录变化 | 代次递增、stale 状态、回调隔离、旧 Binding 拒绝执行、显式重新列举与重新绑定、无自动注册；Handler 通过准入后暂停，并发触发 list_changed + 新 Catalog，恢复旧 Handler 时断言零网络写入 | `ai_sdk_mcp_test` |
| 注册表注销 | 单项/批量、未知名称幂等、重复与空名称预检、定义/Handler/顺序一致、注销后重注册 | 现有 Tool 测试目标 |
| 现有工具链集成 | 绑定注册到 `ToolRegistry`，经 `ToolExecutor` 执行，顺序稳定，单项失败后继续，不触发模型请求 | `ai_sdk_mcp_test` |
| 原子发送 | 前台请求与 ping 响应/取消通知并发；stdio 每行完整、HTTP Session 独立、消息顺序可解释 | `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test`、`ai_sdk_mcp_http_test` |
| 真实子进程 | 初始化、分页、调用、中文与空格路径、参数、环境、工作目录、stderr、提前退出、挂起、正常关闭、生成子孙进程 | `ai_sdk_mcp_stdio_test` |
| Linux 进程边界 | Server 提前关闭 stdin 触发 EPIPE 但宿主存活；独立进程组、孙进程回收、无僵尸进程 | `ai_sdk_mcp_stdio_test` |
| 脚本 HTTP | POST JSON/SSE、202/兼容 204、首次 GET 200/405/401/403/404/5xx、运行期 Listener 的凭据失败/401/403/404/405/其他 3xx/4xx/5xx/错误 Content-Type 状态矩阵、会话、版本头、DELETE 404/405、恢复、取消；`notifications/initialized`、Server 请求响应和 `notifications/cancelled` 分别注入 Provider/超时/401/403/404/5xx/传输失败，断言控制 POST 矩阵中的状态与错误优先级 | `ai_sdk_mcp_test` |
| GET SSE | 默认建流、空闲期请求与通知、ping 响应 POST、注释与空事件保活、405 降级、关闭与重连竞态；分别暂停首次 Listener GET、后台 Listener 恢复 GET、initialize POST 恢复 GET 和 Ready 前台 POST 恢复 GET 的 Provider，断言所需主/流状态；有/无事件 ID 的意外空档、`listChanged` 目录过期、Recovering 期间 list/call 零写入但 Server ping 响应仍成功提交、恢复 Listening 后强制重新列举与绑定、恢复耗尽后工具调用零写入 | `ai_sdk_mcp_test`、`ai_sdk_mcp_http_test` |
| SSE 恢复 | 无事件 ID 的 OutcomeUnknown、Listener GET / 前台恢复 GET / POST 游标隔离、retry 上下界、连续重连重置、前台绝对超时、GET 无绝对寿命、原 POST 次数恒为 1 | `ai_sdk_mcp_test`、`ai_sdk_mcp_http_test` |
| 结果确定性 | NotSubmitted 零写入保留根因；Submitted 后分别注入请求/绝对超时、HTTP 错误、EOF、Server 退出、传输失败、后台 404、队列溢出、协议错误和 close，全部断言 `OutcomeUnknown`、对应 `causeCode`、`mayHaveExecuted=true`、前台终态矩阵中的真实 Client 状态与零重放；非工具 POST SSE 恢复失败保持原始错误；匹配 JSON-RPC result/error 断言结果已知 | `ai_sdk_mcp_test`、两个集成目标 |
| 计时合同 | 可控单调时钟逐项验证前台绝对上限、stdio startup、HTTP connect、单请求段、JSON 正文、SSE 建流/空闲、分页、凭据、retry 退避与 close 的起止；验证子超时不串行相加 | `ai_sdk_mcp_test` |
| 并发准入 | Listener GET SSE 不占前台槽、监听期间可 `callTool()`、Listener GET + 前台恢复 GET 同时存在且 Session/游标隔离、单前台操作、并发调用返回 Busy、控制 POST 不受阻、`close()` 终止前后台任务；分别在 initialized/ping 响应的 Provider 等待、原子提交和 HTTP 等待阶段并发关闭，断言 Closing 线性化优先时最终只进入 Closed 且不写终命失败码 | `ai_sdk_mcp_test` |
| 上层并行 | 两个 Client 可独立调用同一或不同 Server；会话、请求 ID、SSE 游标和关闭互不串扰；SDK 内无隐式 Pool | `ai_sdk_mcp_test` |
| 凭据注入 | 匿名、逐请求刷新、Bearer、预配置 FixedHeader、模式互斥、同 Client 串行门、多 Client 共享 Provider 并发、协作取消、超时、Provider 异常、CR/LF、禁止头名、401/403、Closing 取消排队请求与凭据不可用时跳过 DELETE、秘密不落日志；两阶段准备/提交覆盖 Provider 失败、Provider 等待期间 `list_changed`、并发 `close()`、会话代次变化：`tools/call` 因目录变化销毁且零写入，公开/Server ping 与控制响应不受目录变化阻断 | `ai_sdk_mcp_test` |
| 真实回环 HTTP | cpr 分块回调、动态端口、JSON、分块 SSE、GET 长流、主动断流、响应头、DELETE、关闭竞态 | `ai_sdk_mcp_http_test` |
| 安全 | 无 shell、注入字符按普通参数；HTTPS/loopback 策略、禁重定向、保留头保护、秘密不进 Trace/错误、资源上限；串行测试用 RAII 作用域守卫保存并清空 `NO_PROXY/no_proxy`、设置全套大小写环境代理指向本地陷阱 Server，使用仅在测试 Session 解析到 loopback 且证书匹配的 `mcp.test.local`，断言生产 Session 代理选项为空、目标夹具成功完成最小 MCP 请求、陷阱零连接且无凭据/会话泄漏，所有退出路径恢复并复核进程环境 | 两个集成测试目标 |
| TLS 生产路径 | 生产后端每次设置证书与主机名校验、无关闭入口、无生产自定义 CA/mTLS/代理字段；真实 cpr 连接本地 TLS 夹具时必须完成“自签名拒绝、测试 CA + 正确主机名成功、同一测试 CA + 错误主机名拒绝”对照；明文仅字面量 loopback | `ai_sdk_mcp_test`、`ai_sdk_mcp_http_test`，Windows/Linux 都不得跳过 |
| 资源回收 | 重复启动/关闭、活动后台任务归零、进程/进程组 PID 消失、句柄/FD 无持续增长、测试 Backend 与 Session 析构计数归零 | 两个集成测试目标 |
| Endpoint 信任 | 构造后不可变、无运行期改址入口、模型/Skill/Tool 数据不能建立连接、非法 URL 在 I/O 前拒绝 | `ai_sdk_mcp_test` |
| 构建隔离 | MCP 开启完整构建；关闭后核心与既有测试原样通过；CMake 目标图保持 `ai_sdk_mcp -> ai_sdk` 单向；OFF 时目标不存在且编译数据库无 MCP 源文件；公开头文件无平台类型 | CMake 配置期断言、`compile_commands.json`、`build.ninja`、编译验收 |

测试 Server 至少支持以下确定模式：

```text
normal
paged-tools
chunked-output
stderr-noise
malformed-json
wrong-id
exit-early
hang
tool-error
missing-tools-capability
close-stdin
spawn-child
concurrent-control-message
catalog-change-during-list
catalog-change-during-call
post-json
post-sse
get-sse
get-405
session
session-expired
resume-stream
authentication-failed
redirect
queue-overflow
initialized-post-failure
server-response-post-failure
cancel-notification-post-failure
tls-self-signed
tls-correct-host
tls-hostname-mismatch
proxy-trap
```

资源回收测试预言机：测试 Server 的 `spawn-child` 模式通过测试专用控制通道报告子进程 PID；关闭后在有界期限内断言 Windows Job Object / Linux 进程组内所有 PID 均不存在。内部测试 Backend 使用 RAII 计数器断言活动请求、SSE Worker 和 Session 数归零；重复至少 25 轮连接/关闭，并在 Windows 使用 `GetProcessHandleCount`、Linux 使用 `/proc/self/fd` 验证资源数不持续单调增长。

TLS 运行时验证是强制门禁。夹具使用一套无生产用途的固定测试根 CA、一张由该根签发且仅含 `DNS:mcp.test.local` SAN 的 Server 证书，以及另一张同样仅含 `DNS:mcp.test.local` SAN 的自签名 Server 证书；两张 Server 证书不使用 IP SAN 或通配符。所有证书生成时使用至少 20 年的测试有效期，每次测试在启动 Server 前都必须断言当前时间严格位于各证书 `notBefore` / `notAfter` 内，过期或尚未生效直接以中文诊断失败，不得跳过。Windows 通过 `PFXImportCertStore` + `PKCS12_NO_PERSIST_KEY` 把 PFX 加载到临时内存 Store，使用 SChannel `AcceptSecurityContext` 提供 TLS；Linux 使用 OpenSSL `SSL_accept`。夹具只绑定 loopback 动态端口，不访问公网、不修改用户或系统信任库、不需要管理员权限。

为形成可以单独证明主机名校验的预言机，内部 `IMCPHttpBackend` 提供仅由 `ai_sdk_mcp_http_test` 可见的测试工厂：它可用 libcurl `CURLOPT_CAINFO_BLOB` 把测试根 CA 只注入当前测试 Session，并把 `mcp.test.local` / `mcp-wrong.test.local` 两个名称只在测试 Session 内解析到同一 loopback 夹具。该工厂必须复用生产 cpr Session 构造与请求路径，只允许覆盖测试信任根和测试名称解析，不允许修改 `VerifyPeer=true`、`VerifyHost=true`、空代理、重定向或凭据策略。它不出现在已安装头文件、公共配置、示例或生产目标导出符号中。

强制对照为：（1）不注入测试 CA、访问 `https://mcp.test.local:<动态端口>` 时，真实 cpr 必须以稳定的 libcurl `CURLE_PEER_FAILED_VERIFICATION` 类别拒绝当前有效且 SAN 匹配的自签名 Server；证书有效期、SAN、密钥配对和同一 Server 代码路径均由夹具前置断言保证，因此不解析后端错误文本也能把变量隔离为证书链不受信；（2）注入测试 CA 并访问 `https://mcp.test.local:<动态端口>` 时，TLS 握手和最小 MCP HTTP 交互成功；（3）使用同一测试 CA、同一根签发 Server 和 `https://mcp-wrong.test.local:<动态端口>` 时，也必须以 `CURLE_PEER_FAILED_VERIFICATION` 失败，正确主机名成功对照与唯一变化的访问主机名共同证明主机名校验生效。不得依赖 SChannel / OpenSSL 的错误文本或新增自定义失败阶段枚举。只有失败用例而没有第（2）项成功对照视为无效验证。Windows 或 Linux 任一平台无法完成三项对照时，必须先补足本地验证能力或提交经确认的 PRD 修订，当前任务保持未完成。

### 16.3 本地验收矩阵

| 平台 | MCP | 配置 | 强制结果 |
|---|---:|---|---|
| Windows MSVC | ON | Debug | 配置、编译、全量 CTest 通过 |
| Windows MSVC | ON | Release | 编译通过，全部 MCP 测试目标通过 |
| Windows MSVC | OFF | Debug | 核心 SDK 与全部既有测试通过 |
| Linux GCC | ON | Debug | 配置、编译、全量 CTest 通过 |
| Linux GCC | ON | Release | 编译通过，全部 MCP 测试目标通过 |
| Linux GCC | OFF | Debug | 核心 SDK 与全部既有测试通过 |

验收命令必须使用版本控制内的 `CMakePresets.json`；本地私有 `local-*` 预设只能作为开发快捷方式，不能写入可复现验收证据。四个版本控制预设必须开启 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，用于独立检查 OFF 构建的源文件隔离。执行前由本机环境提供有效 `VCPKG_ROOT`。

CMake 中的 MCP 测试清单固定为 `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test`、`ai_sdk_mcp_http_test` 三个顶层 CTest 条目，三者都必须带 `mcp` 标签；内部 GTest 用例可继续按套件组织。ON 构建必须精确列出这 3 项，OFF 构建必须精确列出 0 项；仅依赖 `ctest` 的零测试成功退出码不构成验收证据。

Windows 三项矩阵：

```powershell
# 先进入已加载 MSVC 的终端，并设置本机 VCPKG_ROOT。
$ErrorActionPreference = 'Stop'
$expectedMcpTests = @('ai_sdk_mcp_test', 'ai_sdk_mcp_stdio_test', 'ai_sdk_mcp_http_test')

function Assert-NativeSuccess([int]$code, [string]$step) {
    if ($code -ne 0) { throw "$step 失败，退出码：$code" }
}

function Assert-McpTestSet([object[]]$output, [string[]]$expected, [string]$step) {
    $actual = @($output | Select-String 'Test\s+#\d+:\s+(\S+)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object)
    $expectedSorted = @($expected | Sort-Object)
    if (($actual -join "`n") -ne ($expectedSorted -join "`n")) { throw "$step 的 MCP CTest 名称集合不匹配" }
    if (-not ($output | Select-String "^\s*Total Tests:\s*$($expected.Count)\s*$")) { throw "$step 的 MCP CTest 数量不匹配" }
}

cmake --preset windows-debug
Assert-NativeSuccess $LASTEXITCODE 'Windows Debug 配置'
cmake --build --preset windows-debug -v
Assert-NativeSuccess $LASTEXITCODE 'Windows Debug 编译'
ctest --preset windows-debug --output-on-failure --no-tests=error
Assert-NativeSuccess $LASTEXITCODE 'Windows Debug 全量测试'
$mcpTests = ctest --test-dir build/windows-debug -N -L mcp
Assert-NativeSuccess $LASTEXITCODE 'Windows Debug MCP 清单查询'
Assert-McpTestSet $mcpTests $expectedMcpTests 'Windows Debug'
ctest --test-dir build/windows-debug -L mcp --output-on-failure --no-tests=error
Assert-NativeSuccess $LASTEXITCODE 'Windows Debug MCP 测试'

cmake --preset windows-release
Assert-NativeSuccess $LASTEXITCODE 'Windows Release 配置'
cmake --build --preset windows-release -v
Assert-NativeSuccess $LASTEXITCODE 'Windows Release 编译'
cmake --build build/windows-release --target ai_sdk_mcp_test ai_sdk_mcp_stdio_test ai_sdk_mcp_http_test -v
Assert-NativeSuccess $LASTEXITCODE 'Windows Release MCP 目标编译'
$mcpTests = ctest --test-dir build/windows-release -N -L mcp
Assert-NativeSuccess $LASTEXITCODE 'Windows Release MCP 清单查询'
Assert-McpTestSet $mcpTests $expectedMcpTests 'Windows Release'
ctest --test-dir build/windows-release -L mcp --output-on-failure --no-tests=error
Assert-NativeSuccess $LASTEXITCODE 'Windows Release MCP 测试'

cmake --preset windows-debug -B build/windows-debug-no-mcp -DAISDK_BUILD_MCP=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
Assert-NativeSuccess $LASTEXITCODE 'Windows OFF 配置'
cmake --build build/windows-debug-no-mcp -v
Assert-NativeSuccess $LASTEXITCODE 'Windows OFF 编译'
ctest --test-dir build/windows-debug-no-mcp --output-on-failure --no-tests=error
Assert-NativeSuccess $LASTEXITCODE 'Windows OFF 全量测试'
$offMcpTests = ctest --test-dir build/windows-debug-no-mcp -N -L mcp
Assert-NativeSuccess $LASTEXITCODE 'Windows OFF MCP 清单查询'
Assert-McpTestSet $offMcpTests @() 'Windows OFF'
$offFiles = @('build/windows-debug-no-mcp/compile_commands.json', 'build/windows-debug-no-mcp/build.ninja')
foreach ($path in $offFiles) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Windows OFF 验收文件不存在：$path" } }
if (Select-String -LiteralPath $offFiles[0] -Pattern '[\\/]src[\\/]mcp[\\/]' -Quiet) { throw "Windows OFF 构建仍编译了 MCP 源文件" }
if (Select-String -LiteralPath $offFiles[1] -Pattern 'ai_sdk_mcp' -Quiet) { throw "Windows OFF 构建仍存在 ai_sdk_mcp 目标" }
```

Linux 三项矩阵：

```bash
set -euo pipefail

expected_mcp_tests="$(printf '%s\n' ai_sdk_mcp_test ai_sdk_mcp_stdio_test ai_sdk_mcp_http_test | sort)"

assert_mcp_tests() {
  local build_dir="$1" expected_count="$2" expected_names="$3" output actual
  output="$(ctest --test-dir "$build_dir" -N -L mcp)"
  printf '%s\n' "$output" | grep -Eq "^[[:space:]]*Total Tests:[[:space:]]*${expected_count}[[:space:]]*$"
  actual="$(printf '%s\n' "$output" | awk '/Test +#[0-9]+:/ { print $NF }' | sort)"
  test "$actual" = "$expected_names" || { printf '%s\n' "${build_dir} 的 MCP CTest 名称集合不匹配" >&2; return 1; }
}

assert_not_contains() {
  local pattern="$1" file="$2" status
  if grep -Eq "$pattern" "$file"; then
    printf '%s\n' "文件包含禁止内容：${file}" >&2
    return 1
  else
    status=$?
    test "$status" -eq 1 || return "$status"
  fi
}

cmake --preset linux-debug
cmake --build --preset linux-debug -v
ctest --preset linux-debug --output-on-failure --no-tests=error
assert_mcp_tests build/linux-debug 3 "$expected_mcp_tests"
ctest --test-dir build/linux-debug -L mcp --output-on-failure --no-tests=error

cmake --preset linux-release
cmake --build --preset linux-release -v
cmake --build build/linux-release --target ai_sdk_mcp_test ai_sdk_mcp_stdio_test ai_sdk_mcp_http_test -v
assert_mcp_tests build/linux-release 3 "$expected_mcp_tests"
ctest --test-dir build/linux-release -L mcp --output-on-failure --no-tests=error

cmake --preset linux-debug -B build/linux-debug-no-mcp -DAISDK_BUILD_MCP=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/linux-debug-no-mcp -v
ctest --test-dir build/linux-debug-no-mcp --output-on-failure --no-tests=error
assert_mcp_tests build/linux-debug-no-mcp 0 ''
test -r build/linux-debug-no-mcp/compile_commands.json
test -r build/linux-debug-no-mcp/build.ninja
assert_not_contains '[/\\]src[/\\]mcp[/\\]' build/linux-debug-no-mcp/compile_commands.json
assert_not_contains 'ai_sdk_mcp' build/linux-debug-no-mcp/build.ninja
```

`ctest -L mcp --no-tests=error` 用于证明 MCP 目标均可筛选执行，显式的 `Total Tests` 断言负责防止标签遗漏造成“零测试假通过”；Debug 的无标签全量 CTest 证明现有核心回归仍通过。OFF 构建还必须同时通过 CMake 配置期构建图断言、`compile_commands.json` 源文件隔离检查和 `build.ninja` 目标不存在检查。

Linux 验收必须在本机 Linux 或本机 WSL 中执行，不能以远程 CI 或人工外包替代。若实现阶段缺少本地 Linux 环境，任务保持未完成，并在任务文档记录阻塞与补偿计划；不得省略矩阵项后宣布完成。

### 16.4 需求到验证追踪

下表为强制合同的唯一追踪入口。实现阶段必须把“计划测试”替换或补充为真实测试文件与 GTest 用例名；任一合同没有本地自动化证据时，任务不得完成。

| 合同编号 | 合同范围 | 主要章节 | 计划验证目标 |
|---|---|---|---|
| `MCP-BLD-001` | 可选目标、默认 ON、OFF 隔离、依赖方向 | 8.1、16.3 | CMake 六项矩阵、配置期目标图断言、三/零 CTest 清单断言、`compile_commands.json` 与 `build.ninja` 隔离检查 |
| `MCP-CFG-001` | 传输二选一、字段校验、默认值与硬上限 | 8.2 | `ai_sdk_mcp_test` 配置用例 |
| `MCP-PROTO-001` | JSON-RPC、版本、能力、请求 ID、取消 | 8.4、8.5 | `mcp_protocol_test`、`mcp_client_test` |
| `MCP-STDIO-001` | 进程启动、逐行 framing、原子写、stderr、关闭 | 8.3.1 | `ai_sdk_mcp_test`、`ai_sdk_mcp_stdio_test` |
| `MCP-HTTP-001` | POST/GET/DELETE、Content-Type、状态码、版本与会话头 | 8.3.2、8.3.4 | `mcp_streamable_http_test`、`ai_sdk_mcp_http_test` |
| `MCP-SSE-001` | 增量解码、游标隔离、POST 恢复、后台 GET 恢复与降级 | 8.3.3 | `mcp_sse_decoder_test`、两个 HTTP 测试目标 |
| `MCP-AUTH-001` | 外部凭据、Provider 串行/截止/取消、Endpoint 信任、TLS 对照、禁环境代理、重定向与保留头 | 8.3.2、8.3.5、12.3 | `mcp_streamable_http_test`、`ai_sdk_mcp_http_test` 本地 TLS 三项对照与代理陷阱、配置审查 |
| `MCP-LIFE-001` | 主状态、Listener 状态、单前台槽、Busy、并发 close | 8.4、10、11 | `mcp_client_test`、两个集成目标 |
| `MCP-TOOL-001` | 分页列举、调用结果、目录代次与竞态 | 8.6、8.7 | `mcp_client_test` |
| `MCP-ADAPTER-001` | 命名、风险、所有权、结果降级与显式注册 | 8.8、8.9 | `mcp_tool_adapter_test`、工具链集成用例 |
| `MCP-REGISTRY-001` | 通用单项/批量注销和确定顺序 | 8.8 | `tests/tool/tool_registry_test.cpp` |
| `MCP-ERROR-001` | 错误码、NotSubmitted / Submitted 提交点、`OutcomeUnknown` 根因、最终状态、发送与零重放语义 | 8.7、11 | `mcp_client_test`、脚本 HTTP/传输用例 |
| `MCP-TIME-001` | 所有超时起止、分页、凭据、SSE 空闲/恢复、退避与 close 总上限 | 8.2 | 可控单调时钟测试、两个集成目标的有界截止 |
| `MCP-SEC-001` | 无 shell、秘密隔离、资源 URI、高风险默认值 | 12 | 两个集成目标、Adapter 测试、敏感哨兵断言 |
| `MCP-OBS-001` | 既有 Tool Trace、无协议正文、日志白名单 | 13 | MCP 工具链集成测试、Trace 敏感哨兵用例 |
| `MCP-RES-001` | 消息/队列/事件上限、线程/句柄/FD/进程回收 | 14、16.2 | 两个集成目标、重复关闭与 OS 资源预言机 |
| `MCP-DOC-001` | README、示例、规格、UTF-8、中文与注释率 | 15、17.3 | 文档检查、编码检查、格式化与注释率记录 |

## 17. 验收标准

### 17.1 功能验收

- [ ] `ai_sdk_mcp` 与核心 `ai_sdk` 保持单向依赖；`AISDK_BUILD_MCP=OFF` 时不编译 MCP 源文件，核心既有行为不变，通用注册表注销 API 独立可用。
- [ ] 本地 C++ 子进程与回环 HTTP Server 都能完成初始化、分页 `tools/list`、`tools/call` 与正常关闭。
- [ ] Streamable HTTP 同时支持 JSON 与 POST SSE 响应、GET SSE、会话头、协议版本头、DELETE 和有限恢复。
- [ ] 初始化后默认尝试 GET SSE；可接收空闲期 Server 请求、通知与保活，`405` 时只降级 GET 能力且不影响 POST JSON/SSE。
- [ ] Listener GET 与前台 POST SSE 恢复 GET 可并存且 Session/游标隔离；运行期 GET 的凭据错误、全部 HTTP 状态和协议错误都匹配闭合 Listener/Client 迁移，应用可通过 `listenerState()` / `lastListenerFailureCode()` 观察。
- [ ] 匿名 Endpoint 与逐请求凭据 Provider 均可使用；同 Client 的 Provider 调用串行、有截止和协作取消，Closing 不因 DELETE 凭据获取突破总上限；SDK 不保存或刷新 Token，`401/403` 不触发自动重放。
- [ ] 只接受协议版本 `2025-11-25`，并严格执行初始化顺序和 Tools 能力检查。
- [ ] `MCPClient` 完整保留成功、`isError` 业务失败和 JSON-RPC 错误的不同语义。
- [ ] 调用方可以筛选 MCP 工具、设置别名并显式注册到 `ToolRegistry`。
- [ ] `tools/list_changed` 或可能丢通知的 Listener 空档使旧 Catalog 与 Binding 立即失效；上层能够批量注销、重新列举和绑定，SDK 不自动改变白名单与注册表。
- [ ] 本地工具、多 Server 工具和显式别名都不会被静默覆盖。
- [ ] MCP 工具能经现有 `ToolExecutor` 串行执行，单项失败不打断同批后续工具。
- [ ] 单个 Client 最多一个用户发起的公开操作在途；GET SSE、通知、`state()` 与 `close()` 不占该名额，多 Client 之间状态完全隔离。
- [ ] 工具调用在原子提交前失败可断言零写入；提交后在匹配终局 JSON-RPC 响应前遇到超时、HTTP 错误、EOF、Server 退出、会话失效、队列/协议错误或 close 时，一律返回带脱敏根因和 `mayHaveExecuted=true` 的 `OutcomeUnknown`，任何层都不自动重放。
- [ ] 可控单调时钟证明计时表中每一个起点、终点、重置和包含关系，分页、凭据、SSE 保活与重连不能无限延长前台操作。
- [ ] 非法 JSON、错误 ID、超时、Server 提前退出、EOF、HTTP 状态、SSE 断线、会话失效、队列溢出和富媒体 Adapter 路径分别匹配第 11 节错误码、Client/Listener 最终状态及“是否发送/重放请求”合同。
- [ ] 不实现 Skill，也不修改 `AIClient` 形成自动 Agent Loop。

### 17.2 安全与资源验收

- [ ] 子进程启动不经过 shell，命令注入字符只作为普通参数传递。
- [ ] stdout 与 stderr 完全分离；stderr 写满不会造成死锁，也不会进入协议、Trace 或公开错误。
- [ ] 超时、关闭和析构后无线程、句柄、文件描述符、僵尸进程或受控子孙进程残留。
- [ ] HTTP 关闭后无残留 GET/POST、重连线程、cpr Session 或消息队列等待者。
- [ ] Server 命令、参数、环境变量值、Endpoint 查询参数、鉴权头、会话 ID、事件 ID和协议大正文不进入 Trace 或默认日志。
- [ ] 消息大小、SSE 事件、队列、重连、分页、工具数量、错误文本和 stderr 缓存均有自动测试覆盖的上限。
- [ ] TLS 校验不可关闭，Windows/Linux 真实 cpr 路径均通过本地夹具完成“自签名拒绝、测试 CA + 正确主机名成功、同 CA + 错误主机名拒绝”对照；远程明文 HTTP 与自动重定向默认拒绝，开发用 HTTP 仅允许显式字面量回环 IP。
- [ ] 代理陷阱测试串行运行，用 RAII 作用域守卫逐项保存大小写 HTTP/HTTPS/ALL_PROXY 与 `NO_PROXY/no_proxy` 的“未设置/原值”状态，清空两种 NO_PROXY 后设置陷阱代理；`mcp.test.local` 只由测试 Session 解析到正确证书的 loopback 夹具。每个生产 cpr Session 的代理选项必须显式为空，目标夹具成功完成一次最小 MCP 请求，陷阱零连接，凭据、会话 ID 与游标不泄漏；作用域守卫在正常、断言失败、异常和超时退出路径都恢复环境，并在析构后复核与测试前完全一致。
- [ ] Endpoint 只从应用受信配置进入，构造后不可变；模型、Skill、Tool 结果和 Server 消息没有动态连接入口。
- [ ] 凭据 Provider 不能覆盖保留头，任何 Token、API Key 或 Provider 异常细节都不会进入 Trace、默认日志或公开错误。
- [ ] 所有远端工具默认高风险，Server 注解不能自动降低风险。

### 17.3 工程质量验收

- [ ] Windows MSVC 与 Linux GCC 的 MCP 开启 / 关闭本地矩阵全部通过。
- [ ] 所有新增测试可重复、非跳过、无公网和外部运行时依赖。
- [ ] 新增代码通过项目格式化及 `/W4` 或 `-Wall -Wextra -Wpedantic`。
- [ ] 所有可自由书写的新增文本、文档、注释、日志和测试描述使用简体中文；协议方法、HTTP 头、标准错误码、代码标识符与第三方 API 字面量遵循规范原文。C++ 文件为 UTF-8 无 BOM。
- [ ] 本任务按用户于 2026-07-15 确认的口径计算注释率：新增 C++ 文件中“去除纯空行后，以 `//` 开头的注释行 / 非空行”不低于 20%；实现任务必须记录文件列表、注释行数、非空行数和计算结果。
- [ ] README、跨平台构建文档、SDK 合同、错误合同、目录规范和测试清单与最终 API 同步。
- [ ] 示例目录为 `examples/08_mcp_tool_call/`，展示显式连接、筛选、注册、执行和关闭，不包含 API Key 或自动模型循环。

## 18. 实施阶段拆分

用户确认本 PRD 后，按以下小步进入实现，每一阶段都必须保持可编译、可测试：

1. **协议模型与脚本传输**
   建立 `ai_sdk_mcp` 空目标、公共类型、状态机和假传输；完成 JSON-RPC、生命周期、分页和错误单元测试。
2. **跨平台 `stdio` 传输**
   实现 Windows / Linux 进程层和 C++ 测试 Server；完成进程、编码、stderr、超时及资源回收测试。
3. **Streamable HTTP 传输**
   实现专用 HTTP 后端、增量 SSE、GET/POST/DELETE、会话、恢复、网络安全策略和回环 Server；完成真实 cpr 集成测试。
4. **Tool Adapter 与现有链路集成**
   完成命名、冲突、风险、弱引用和结果映射；通过 `ToolRegistry` / `ToolExecutor` 离线集成测试。
5. **文档、示例与完整质量门**
   新增 `08_mcp_tool_call`，更新 README 与 Trellis 规格，执行双平台开启 / 关闭矩阵。

任何阶段连续三次出现同类失败时，暂停实现并重新评估设计或验证环境，不以绕过测试的方式继续。

## 19. 技术选型理由

### 19.1 选择“窄协议子集 + 两个标准传输”

- **为什么用这个方案**：用户已确认首版同时覆盖本地 `stdio` 与远程 Streamable HTTP；官方当前没有 C++ SDK，而项目只需要 MCP Tools，不需要完整 Server 与其他能力。现有 JSON、cpr、线程和平台构建能力足以实现可审计的客户端。
- **优势**：不新增生产依赖；本地与远程 Server 使用同一 `MCPClient` 和 Tool Adapter；协议、进程、HTTP、SSE 与适配可以分层测试；核心 SDK 不被迫感知 MCP。
- **劣势和风险**：除平台进程边界外，还需要维护 HTTP 会话、增量 SSE、断线恢复、鉴权注入与关闭竞态；实现和双平台验证规模显著扩大。

### 19.2 复用 cpr 但不复用 Provider HttpClient

- **为什么用这个方案**：仓库已经使用 cpr，它可以继续承担 HTTPS、证书校验和跨平台网络 I/O；但现有 `HttpClient` 缺少 GET、DELETE、响应头、取消和长流语义，现有 `SSEParser` 也不保留恢复字段。
- **优势**：不破坏 Provider 接口隔离，不让长期 SSE 无界累计正文，也不依赖当前 vcpkg 未锁定版本的高层 SSE API。
- **劣势和风险**：MCP 模块需要新增内部 HTTP Backend 与 SSE Decoder；底层仍需审计 cpr 回调、取消和多 Session 关闭行为。

### 19.3 不选择 Boost.Process

- **为什么不选**：当前仓库没有 Boost；为了一个窄进程边界引入 Boost.Process 与相关依赖会增加 vcpkg 安装、构建体积和版本维护成本。
- **保留条件**：若原生进程层在实现或审计中反复失败，且 Boost.Process 能通过本地依赖、体积和双平台验证，再另立决策记录重新评估。

### 19.4 不选择社区完整 C++ MCP SDK

- **为什么不选**：没有官方 C++ SDK可以提供版本与安全维护承诺；社区实现通常扩大到本任务明确排除的能力，并引入新的 API 和依赖风险。
- **保留条件**：候选库支持目标协议、进入项目可接受的包管理源、完成许可证与安全审计，并能显著减少本地维护成本后再评估。

## 20. 关键风险与应对

| 风险类型 | 风险 | 应对与验收 |
|---|---|---|
| 并发问题 | EOF、HTTP 取消、Listener / 前台恢复 GET、凭据 Provider、超时、`close()` 与后台线程竞争，可能重复完成请求或永久等待 | 单公开请求；两类 GET 独立 Session/游标；Provider 串行门与协作取消；闭合状态机；完成操作只允许一次；有界队列与有限 join；竞态单元测试 |
| 边界条件 | stdio 分帧、SSE 跨块、空事件、重复游标、会话 404、恢复后迟到响应 | 脚本传输、脚本 HTTP 后端和两个真实测试 Server 覆盖 |
| 性能瓶颈 | 大 Schema、富媒体、长期 SSE、大 stderr 导致内存增长 | 消息、事件、队列、重连、页数、工具数、错误和 stderr 全部有界 |
| 安全考虑 | shell 注入、父环境秘密泄露、环境代理劫持、凭据跨源泄漏、恶意 Endpoint、进程树残留 | 无 shell、HTTPS、每 Session 空代理、代理陷阱、禁重定向、保留头保护、默认高风险、Job Object / 进程组 |
| 副作用不确定 | 工具请求已提交后遇到超时、断线、会话失效或关闭，上层把普通失败误当成可重试 | 单一原子提交点；Submitted 后无终局 JSON-RPC 统一 `OutcomeUnknown`；脱敏根因与 `mayHaveExecuted=true`；各层零自动重放 |
| 兼容风险 | 目标协议升级或 Server 只支持其他修订 | 首版精确支持单一版本；不伪装兼容；后续按独立任务升级 |
| 结果损失 | 现有失败 `ToolResult` 无法保存结构化正文 | Client 结果无损；Adapter 文档化有损边界；不偷改核心合同 |
| 工具覆盖 | `ToolRegistry` 同名注册会替换已有实现 | Server 命名空间、显式别名、整批预检、默认拒绝冲突 |
| 可用性 | Windows 常见 `.cmd` 启动器不能直接执行 | 文档要求显式传入真实解释器或可执行程序；首版不以 shell 换便利性 |

## 21. 迁移、回滚与后续演进

### 21.1 迁移

这是新增可选模块，不改变现有核心调用方式。现有用户无需修改；需要 MCP 的用户显式链接 `ai_sdk_mcp`，自行持有 Client，并把选定绑定注册到现有 `ToolRegistry`。

### 21.2 回滚

- 构建期设置 `AISDK_BUILD_MCP=OFF` 即可完全排除 MCP 源文件和测试目标。
- 因核心 `ai_sdk` 不反向依赖 MCP，回滚 MCP 模块不需要修改 `AIClient`、Provider 或既有工具执行链路。
- 若平台进程实现存在未解决缺陷，该平台的 MCP 功能不得带缺陷交付；任务保持未完成，而不是用远程 CI 或关闭测试代替验证。

### 21.3 后续独立任务候选

- MCP Resources 与 Prompts。
- 完整 MCP OAuth 2.1 授权、Token 存储和刷新。
- 富媒体 ToolResult 或通用内容块合同。
- 显式 `ToolExecutionContext` 与 MCP 子级 Trace。
- Skill Loader 与 Skill Registry；它继续属于 Agent 层，不并入 MCP 协议模块。

## 22. 边界决策记录与当前问题

已确认：

- `AIClient` 不直接持有 MCP 连接；Agent 或应用持有 Client，并把选定工具显式接入现有注册表。
- 首版同时实现本地 `stdio` 和远程 Streamable HTTP，不兼容旧 HTTP + SSE。
- Streamable HTTP 支持 JSON、POST SSE、GET SSE、会话、DELETE 与有限恢复，但不重放原始 `tools/call`。
- Streamable HTTP 采用外部逐请求凭据注入；同 Client 的 Provider 调用通过凭据门串行，必须在协作截止时间内返回并响应关闭取消。应用负责 Token 获取、刷新与保存，SDK 不实现完整 MCP OAuth 2.1。
- 远程 Endpoint 只允许由应用受信配置在构造 Client 时提供；Client 生命周期内不可变，Agent 或模型不能动态连接任意地址。
- HTTP 会话 `404` 后，旧 Client 失效并清理，由上层显式创建新 Client、重新列举和绑定；SDK 不自动建立新会话，也不重放当前调用。已提交且无终局响应的工具调用返回 `OutcomeUnknown(cause=SessionExpired)`。
- 初始化完成后默认建立最多一条后台 GET SSE 监听流，用于空闲期 Server 请求、通知和保活；前台 POST SSE 断线时可另外建立一条独立恢复 GET，两者游标不混用。后台 GET 返回 `405 Method Not Allowed` 正常降级为仅处理 POST JSON/SSE，不影响其他 MCP 功能。
- SSE 流在 Server 提供事件 ID 时执行有限恢复：POST 响应流同时受连续重连次数和前台绝对请求超时限制，后台 GET 仅受连续重连与退避上限约束，不设置两分钟生命周期。两者都只续接流，绝不重新 POST 原始 `tools/call`；更广泛的 Submitted 后结果丢失也统一返回带根因的 `OutcomeUnknown`。
- 一个 `MCPClient` 对应一个 Server 配置和一个逻辑会话；最多一个用户发起的公开操作在途，GET SSE、通知处理、`state()`、`close()`、stdio 读取、取消和控制 POST 均不占该名额。多 Server 或并行工具调用由上层使用多个 Client 或 Client Pool；首版 SDK 不实现 Pool。
- `MCPClient` 只返回远端工具目录；Agent 或应用按白名单筛选工具、设置别名与风险等级，再通过 Adapter 显式注册。所有远端工具默认高风险，SDK 不自动全量同步或执行。
- `tools/list_changed` 或已建立 Listener 的意外空档递增目录代次，旧 Catalog 与 Binding 立即返回 `ToolCatalogStale`；调用携带 Client 签发的 Catalog 并在发送提交时原子校验代次。上层显式批量注销、重新列举、筛选和绑定，SDK 不自动刷新注册表。
- `MCPToolCallResult` 无损保留全部 MCP 内容块；适配现有 `ToolResult` 时只支持文本与对象 `structuredContent`，图片、音频、资源链接和内嵌资源返回固定中文“不支持 MCP 富媒体工具结果”失败，不直接暴露 Base64、URI 或资源正文。
- `stdio` 只接受真实可执行文件的绝对路径与参数数组，不经过 shell，不直接执行 Windows `.cmd` / `.bat`；脚本 Server 由应用显式指定解释器可执行文件和脚本路径。
- Streamable HTTP 始终启用 TLS 证书与主机名校验并使用系统信任库；生产首版不支持自定义 CA、mTLS、代理、代理认证或关闭校验，每个 cpr Session 显式禁用环境代理。测试专用 CA 只能通过未导出的内部工厂注入单个测试 Session，用于形成正确/错误主机名对照，不属于产品能力。
- `AISDK_BUILD_MCP` 默认 `ON`，标准 Windows/Linux 构建持续编译和测试独立 `ai_sdk_mcp`；应用仍需显式链接该目标，也可设置为 `OFF` 排除全部 MCP 源文件。

当前不再保留未决技术边界，整份 PRD 已确认并作为实现与验收的唯一需求基线。

用户已确认整份 PRD，任务状态已切换为 `in_progress`；实现阶段以本文件为需求基线并按第 18 节小步推进。
