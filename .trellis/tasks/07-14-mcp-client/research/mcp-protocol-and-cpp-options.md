# MCP 协议与 C++ 实现选型研究

## 研究范围

本研究服务于 `07-14-mcp-client` 的首版 MCP Client PRD，只评估 MCP 基础协议、生命周期、`stdio` / Streamable HTTP 传输、工具发现与调用、错误和安全边界，以及当前 C++17 仓库可采用的实现方式。Skill、MCP Server、Resources、Prompts、Sampling、Elicitation、Roots 与实验性 Tasks 不属于本次范围。

检索日期：2026-07-14。

## 一、官方协议事实

### 1. 当前稳定修订版

- 当前官方最新稳定协议修订版为 `2025-11-25`。
- MCP 基础协议使用 UTF-8 JSON-RPC 2.0；请求 ID 必须是字符串或整数、不得为 `null`，并且同一会话内不得重复使用。
- 所有实现必须支持基础协议和生命周期；其他能力根据应用需要选择实现。

来源与用途：

- [MCP 基础协议概览](https://modelcontextprotocol.io/specification/2025-11-25/basic/index)：确认 JSON-RPC 消息约束、协议必选层和鉴权适用范围。
- [MCP 生命周期](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)：确认初始化、版本协商、能力协商、运行、关闭和超时要求。

### 2. 初始化与能力协商

连接后的第一次协议交互必须是 `initialize`。客户端发送其协议版本、能力和实现信息；服务端返回协商版本、能力和实现信息。客户端成功收到响应后必须发送 `notifications/initialized`，之后才能进入正常操作。

本项目首版客户端不声明 Roots、Sampling、Elicitation 或 Tasks 能力，只消费服务端声明的 `tools` 能力。若服务端返回的协议版本不在客户端支持集合内，客户端关闭连接并报告版本不兼容；若服务端没有声明 `tools`，`listTools()` 与 `callTool()` 必须拒绝执行。

### 3. 标准传输

官方当前定义两种标准传输：

- `stdio`：客户端启动 Server 子进程，通过标准输入和标准输出交换逐行 UTF-8 JSON-RPC 消息；每条消息由换行符分隔且内部不得包含换行。标准错误只用于日志，客户端不能把存在标准错误输出等同于协议失败。
- Streamable HTTP：单一 MCP 端点同时支持 POST 和 GET，可通过 SSE 返回多条消息，并涉及会话头、恢复、重投递、版本头、鉴权和 SSRF 等额外安全问题。

官方建议客户端在可能时支持 `stdio`。考虑本项目当前使用同步 `ToolHandler`、尚无通用双向 HTTP/SSE 客户端，以及首版需要可控的本地自动化测试，推荐第一阶段只实现 `stdio`，把 Streamable HTTP 明确留到后续任务。

来源与用途：

- [MCP 传输规范](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)：确认逐行 framing、标准错误语义、两种标准传输以及 HTTP 的额外复杂度。

### 4. 工具能力

- `tools/list` 支持游标分页；客户端必须循环读取 `nextCursor`，并设置页数、工具数和消息大小上限，避免恶意或错误 Server 造成无界增长。
- `tools/call` 接收工具名和 JSON 对象参数。
- 工具定义包含 `name`、可选 `title` / `description`、必需的对象 `inputSchema`，以及可选 `outputSchema`、`annotations`、`icons` 和 `execution`。
- 工具结果可能包含文本、图片、音频、资源链接、内嵌资源和 `structuredContent`。
- 协议错误通过 JSON-RPC `error` 返回；可供模型修正的工具执行错误通过结果内 `isError: true` 返回。两者不能混为一类。
- 工具注解来自不受信任的 Server，不能直接用于降低风险等级或绕过审批。

首版 SDK 工具适配只承诺完整支持文本块和对象形式的 `structuredContent`。MCP Client 本身保留完整结果对象；适配到现有 `ToolResult` 时，图片、音频、资源链接和内嵌资源返回明确的“不支持内容类型”失败，禁止把大段 Base64 静默塞入模型消息。

来源与用途：

- [MCP Tools 规范](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)：确认分页、工具 Schema、结果类型、错误分类和用户确认要求。

### 5. 超时、取消与关闭

- 所有请求都应有可配置超时和绝对最大超时。
- 普通请求超时后应发送 `notifications/cancelled` 并停止等待；`initialize` 不允许由客户端取消。
- `stdio` 关闭顺序为：关闭子进程输入、等待正常退出、超时后请求终止、仍不退出则强制终止。
- Server 主动关闭输出或退出时，所有待处理请求必须以确定的传输关闭错误结束。

来源与用途：

- [MCP 生命周期](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)：确认关闭与超时规则。
- [MCP 取消规范](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation)：确认取消通知、竞态和无响应语义。

## 二、C++ SDK 与依赖现状

### 1. 没有官方 C++ SDK

官方 SDK 列表当前包含 TypeScript、Python、C#、Go、Java、Rust、Swift、Ruby、PHP 和 Kotlin，没有 C++。因此首版不能依赖一个由官方维护、协议版本与安全修复有明确承诺的 C++ SDK。

来源与用途：

- [MCP 官方 SDK 列表](https://modelcontextprotocol.io/docs/sdk)：确认不存在官方 C++ SDK，避免把社区项目误写成官方依赖。

### 2. 可选实现路线

#### 方案 A：仓库内实现窄协议子集与原生 `stdio` 传输（推荐）

实现方式：

- 使用现有 `nlohmann/json` 编解码 JSON-RPC。
- JSON-RPC、生命周期、工具模型和适配器保持平台无关。
- `stdio` 子进程与管道只在独立传输实现中按 Win32 / POSIX 分文件封装。
- 公开 API 只暴露 SDK 自有类型和 `IMcpTransport`，不暴露平台句柄。

优势：

- 不引入新的运行时或大体积依赖。
- 完全匹配本任务只做 Tools 的窄范围。
- 可通过脚本传输和本地 C++ 测试 Server 做确定性测试。

劣势与风险：

- Windows 命令行参数、句柄继承、UTF-8 / UTF-16 转换以及 POSIX 管道关闭都需要自行正确实现。
- 协议升级需要项目持续维护。

#### 方案 B：协议自行实现，进程层采用 Boost.Process

实现方式：使用 Boost.Process 管理跨平台子进程、管道和终止，协议层仍由项目实现。

优势：减少平台进程代码和句柄继承错误；Boost.Process 支持 Windows / Linux，并能与异步管道结合。

劣势与风险：引入 Boost.Process 会连带 Boost.Asio 等依赖，增加 vcpkg 安装、构建体积和版本管理成本；当前仓库尚未使用 Boost。

来源与用途：

- [Boost.Process 官方文档](https://www.boost.org/doc/libs/latest/libs/process/doc/html/index.html)：评估进程、管道、终止和异步支持。
- [Boost.Process 库页](https://www.boost.org/library/latest/process/)：评估语言标准与依赖范围。

#### 方案 C：引入社区完整 C++ MCP SDK

优势：理论上能更快覆盖更多 MCP 能力。

劣势与风险：当前没有官方 C++ SDK；社区实现的协议修订、API 稳定性、依赖、测试与维护承诺不一致，而且完整 SDK 会把本任务明确排除的 Server、Resources、Prompts、Sampling 等能力一并带入。

结论：首版不采用。只有当社区库形成稳定版本、支持项目所需协议修订、进入 vcpkg、通过本地审计并显著减少维护成本时再重新评估。

## 三、与当前仓库的映射

### 1. 可直接复用

- `Tool`：映射 MCP 工具名、描述和 `inputSchema`。
- `ToolHandler`：包装同步 `tools/call`。
- `ToolRegistry`：保存适配后的定义和处理函数。
- `ToolExecutor`：按当前顺序执行模型返回的 MCP 工具调用。
- `IHttpTransport` 的依赖注入模式：作为 `IMcpTransport` 设计与确定性测试的参考，但不能复用其单次 HTTP POST 语义。
- 现有 Tool Trace：至少记录适配后的工具名、成功状态与批次统计。

### 2. 不能直接复用

- `IHttpTransport` 只表达 JSON POST / SSE，无法表达双向、长生命周期的 `stdio` JSON-RPC 会话。
- `AIClient` 只负责模型和本地工具门面，不应持有、自动启动或扫描 MCP Server。
- `Config` 是模型 Provider 顶层配置；MCP Server 配置应由独立 `McpServerConfig` 承载。
- 当前 `ToolResult` 不能完整表达 MCP 富媒体内容，因此 Client 结果模型与 Tool 适配结果必须分层。
- 当前 `ToolHandler` 没有 Trace 上下文，首版不能伪造 MCP 子步骤或依赖线程局部状态。

## 四、研究结论

推荐采用方案 A：实现独立的 `ai_sdk_mcp` 目标，首版固定支持协议修订 `2025-11-25` 和本地 `stdio`，使用现有 JSON 依赖并隔离 Win32 / POSIX 进程细节。公开 API 保持同步，以匹配当前 `ToolHandler`；内部使用单独读取线程处理响应、通知和 Server 请求。Streamable HTTP、旧协议修订、热更新、富媒体 ToolResult 适配和 MCP 专用 Trace 留到后续任务。

该方案的核心取舍是：以较小而可审计的实现范围换取零新增依赖；代价是平台进程层和协议升级需要由本项目负责维护。

## 五、用户决策补充：首版纳入 Streamable HTTP

决策日期：2026-07-14。

用户确认首版同时实现本地 `stdio` 与远程 Streamable HTTP。因此，上一节“只做 `stdio`”保留为原始研究建议，但不再是任务最终范围；最新范围以 `../prd.md` 为准。

### 1. 官方传输合同补充

目标协议仍固定为 `2025-11-25`，Streamable HTTP 首版需要覆盖：

- 每条 JSON-RPC 消息使用独立 POST，且请求声明同时接受 JSON 与 SSE。
- JSON-RPC 请求的 POST 响应同时支持 `application/json` 和 `text/event-stream`。
- Client 通知和对 Server 请求的响应使用 POST，规范成功响应为 `202 Accepted` 空正文。
- 初始化后支持 GET SSE；Server 返回 `405` 时正常降级。
- 保存每条 SSE 流自己的 `id` 和 `retry`，断线时通过 GET + `Last-Event-ID` 有限恢复。
- 初始化响应若分配 `MCP-Session-Id`，后续 POST、GET、DELETE 都携带该值；初始化后的请求携带协商版本头。
- Client 结束会话时尽力 DELETE；会话 `404` 后重新初始化，但不得自动重放可能有副作用的工具调用。
- 不实现已经废弃的旧 HTTP + SSE 探测与回退。

来源与用途：

- [MCP Streamable HTTP 传输规范](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)：确认 POST / GET、JSON / SSE、会话、恢复、协议版本头和关闭语义。

### 2. 当前仓库复用结论

- 继续复用 cpr 作为 HTTPS 与跨平台网络后端，不新增生产依赖。
- 不直接复用或扩展现有 `HttpClient`：其响应不含响应头，接口没有 GET、DELETE、取消和长连接会话语义。
- 不复用 DeepSeek `SSEParser`：它只提取 `data:`，不保留 MCP 恢复需要的 `id` 与 `retry`，也不负责跨网络分块状态。
- 在 `ai_sdk_mcp` 内新增专用 Streamable HTTP 传输、内部 HTTP Backend 测试缝和有状态 SSE Decoder；cpr 类型继续封闭在 `.cpp`。

### 3. 新增安全与测试范围

- Endpoint 只能来自应用受信配置，默认只允许 HTTPS；明文 HTTP 仅显式允许 loopback 测试。
- TLS 校验不可关闭，首版禁止自动重定向，敏感头、会话 ID 和事件游标不得进入 Trace 或日志。
- 使用本地动态端口 C++ HTTP/1.1 回环 Server 验证真实 cpr 分块、响应头、JSON、SSE、GET、DELETE、取消和关闭，不以公网 Server 代替。
- 用户已确认 HTTP 鉴权采用外部逐请求凭据注入：应用负责 Token 或 API Key 的获取、刷新与安全保存；SDK 只在每个 POST、GET、恢复 GET 和 DELETE 前获取当前凭据，并对 `401/403` 分类返回。首版不实现 Protected Resource Metadata、OAuth/OIDC 发现、PKCE、浏览器回调、Token 存储或自动 Scope 升级，也不宣称完整 MCP OAuth 兼容。
- 用户已确认远程 Endpoint 只从应用受信配置进入，构造 `MCPClient` 后不可变；模型、Skill、MCP Tool 结果与 Server 消息不能动态创建连接或修改目标地址。
- 用户已确认 HTTP 会话 `404` 后由上层显式恢复：旧 Client 失效并清理，应用创建新 Client 后重新初始化、列举与绑定；SDK 不自动重建会话，也不重放原始调用。最终 PRD 进一步明确：普通操作返回 `SessionExpired`，已提交且没有终局 JSON-RPC 响应的工具调用返回带该根因的 `OutcomeUnknown`。
- 用户已确认初始化后默认尝试维护一条 GET SSE 监听流，用于会话空闲期的 Server 请求、通知与保活；Server 返回 `405 Method Not Allowed` 时正常降级为仅处理 POST JSON/SSE，不影响其他 MCP 功能。
- 用户已确认 SSE 流执行基于事件 ID 的有限恢复：遵守 Server `retry`，通过 GET + 当前流自己的 `Last-Event-ID` 续接；POST 响应流受重连次数与前台绝对超时约束，后台 GET 不设置前台两分钟寿命、只限制连续失败与退避。两者都不重新 POST 原始工具调用；最终 PRD 将 `OutcomeUnknown` 扩展为所有“工具请求已提交但无终局 JSON-RPC 响应”的统一保守语义。
- 用户已确认把并发拆成前后台两层：一个 `MCPClient` 对应一个 Server 配置和一个逻辑会话；GET SSE、重连、stdio 读取、Server 消息和控制 POST 属于 Transport 后台任务，不占“一个公开操作在途”的前台槽。多 Server 由上层按 `server_id` 持有多个 Client。
- 用户进一步明确：单 Client 最多一个用户发起的公开操作在途，后台 GET SSE、通知、状态读取和 `close()` 不计入限制；并行工具调用由上层使用多个 Client 或 Client Pool，首版 MCP SDK 不内置 Pool。
- 用户已确认 MCP 工具由上层显式筛选与注册：Client 只返回完整目录，上层决定白名单、别名与风险，SDK 不自动全量暴露。
- 用户已确认 `tools/list_changed` 采用目录版本失效：Client 递增代次，旧 Catalog 与 Binding 返回 `ToolCatalogStale`；上层显式批量注销、重新列举与绑定，SDK 不自动刷新注册表。为支持安全清理，本任务给通用 `ToolRegistry` 增加批量注销能力。
- 用户已确认 MCP 富媒体结果采用分层合同：`MCPToolCallResult` 无损保留全部内容块，现有 Tool Adapter 只接受文本与对象 `structuredContent`；图片、音频和资源块返回固定中文不支持错误，不向模型直接暴露 Base64 或资源 URI。
- 用户已确认本地 `stdio` 只接受真实可执行文件的绝对路径和参数数组，不经过 shell，不直接执行 Windows `.cmd` / `.bat`；脚本 Server 由应用显式配置解释器和脚本路径。
- 用户已确认 Streamable HTTP 始终启用 TLS 证书与主机名校验并使用系统信任库；首版不支持自定义 CA、mTLS、HTTP(S) 代理、代理认证或关闭校验。
- 用户已确认 `AISDK_BUILD_MCP` 默认 `ON`，标准 Windows/Linux 构建持续编译和测试独立 MCP 目标；使用者仍需显式链接，也可设置为 `OFF` 排除 MCP 源文件。

### 4. TLS 夹具与代理隔离补充

为使 PRD 中的 TLS 与环境代理门禁可在无公网、无管理员权限的 Windows/Linux 上重复验证，最终方案增加以下仅测试边界：

- Windows TLS Server 夹具使用 SChannel；`PFXImportCertStore` 返回临时 Store，`AcceptSecurityContext` 是 Server 端 TLS 安全上下文握手入口。
- Linux TLS Server 夹具使用 cpr SSL 依赖图已提供的 OpenSSL，通过 `SSL_accept` 完成 Server 握手，不新增生产依赖。
- 主机名验证必须有“同一测试 CA 下正确主机名成功”的对照；测试 CA 只通过未导出内部工厂向单个测试 Session 注入。libcurl 的 `CURLOPT_CAINFO_BLOB` 支持 OpenSSL 与 Schannel，因此不需修改系统信任库。
- libcurl 默认会读取环境代理，`NO_PROXY/no_proxy` 又能绕过代理；将 `CURLOPT_PROXY` 设为空字符串可显式禁用环境代理。因此生产后端必须对每个 Session 设置空代理，代理陷阱测试还必须清空两种 `NO_PROXY` 拼写并使用测试 Session 内解析的合成主机，避免回环地址被环境规则绕过而造成假阳性。

来源与用途：

- [Microsoft `PFXImportCertStore`](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-pfximportcertstore)：确认 PFX 可加载为临时证书 Store，不需安装到用户或系统信任库。
- [Microsoft SChannel `AcceptSecurityContext`](https://learn.microsoft.com/en-us/windows/win32/secauthn/acceptsecuritycontext--schannel)：确认 Windows 本地 TLS Server 握手和安全上下文边界。
- [OpenSSL `SSL_accept`](https://docs.openssl.org/master/man3/SSL_accept/)：确认 Linux 本地 TLS Server 握手边界。
- [libcurl `CURLOPT_CAINFO_BLOB`](https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html)：确认内存 PEM CA 注入支持 OpenSSL 与 Schannel，用于仅测试的正确/错误主机名对照。
- [libcurl 错误码](https://curl.se/libcurl/c/libcurl-errors.html)：确认 `CURLE_PEER_FAILED_VERIFICATION` 是跨后端稳定的对端证书校验失败类别；测试通过受控证书变量区分不受信链与主机名错误，不依赖后端错误文本。
- [libcurl `CURLOPT_PROXY`](https://curl.se/libcurl/c/CURLOPT_PROXY.html)：确认空代理字符串会显式禁用环境代理。
- [libcurl 环境变量说明](https://curl.se/libcurl/c/libcurl-env.html)：确认 `NO_PROXY/no_proxy` 会改变代理绕过行为，用于设计不会假通过的本地代理陷阱。
