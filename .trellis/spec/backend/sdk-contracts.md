# SDK 契约

## 场景一：`AIClient` 到 Provider 的聊天契约

### 1. 作用范围 / 触发条件

- 触发：修改 `AIClient`、`IModelProvider`、`DeepSeekProvider`，或新增其他 Provider。
- 目标：保证同步聊天与流式聊天在公开 API、配置读取和错误传播上保持一致。

### 2. 关键签名

- `ChatResponse AIClient::chat(const ChatRequest& request)`
- `ChatResponse AIClient::chat(const ChatRequest& request, TraceSession& trace_session)`
- `void AIClient::streamChat(const ChatRequest& request, StreamCallback callback)`
- `void AIClient::streamChat(const ChatRequest& request, StreamCallback callback, TraceSession& trace_session)`
- `virtual ChatResponse IModelProvider::chat(const ChatRequest& request) = 0`
- `virtual void IModelProvider::streamChat(const ChatRequest& request, StreamCallback callback) = 0`
- `void AIClient::setProvider(std::shared_ptr<IModelProvider> provider)`

### 3. 契约

- `AIClient` 构造时接收完整 `Config`，并按 `default_provider` 选择 Provider。
- `Config::providers` 必须包含目标 Provider 的配置；当前仅支持键名 `deepseek`。
- `ChatRequest` 必须保留消息顺序，`messages` 为空时依然属于调用方错误输入。
- `chat()` 返回完整 `ChatResponse`；`streamChat()` 通过回调逐步返回文本增量或工具增量。
- `streamChat()` 允许空回调时直接返回，不主动发请求。
- Trace 重载只增加结构化旁路步骤，原返回值、异常和回调协议不变；详细约束引用 [Trace 契约](./trace-contracts.md)。
- 自定义 Provider 实例必须非空且 `info().name` 非空，所有校验完成后才能替换当前实例。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| `default_provider` 为空或 `setProvider("")` | 抛 `std::invalid_argument` |
| Provider 名不存在或不支持 | 抛 `std::invalid_argument` |
| Provider 未创建成功 | 后续 `chat` / `streamChat` 抛 `std::logic_error` |
| Provider 运行期失败 | 透传 `std::runtime_error` 或底层异常 |

### 5. Good / Base / Bad

- Good：`Config` 中声明 `deepseek`，`ChatRequest` 含有合法消息数组，`chat()` 或 `streamChat()` 正常转发到底层 Provider。
- Base：只依赖最小配置 `api_key` + 默认模型，未传 `tools`，仍能完成请求。
- Bad：在 `AIClient` 里硬编码第二套请求 JSON，绕过 Provider 契约。

### 6. 必要测试

- 冒烟测试验证默认 Provider 选择与不支持 Provider 的报错。
- Provider 替换测试验证 `setProvider()` 先创建后切换，失败时不污染当前状态。
- 流式聊天测试至少覆盖空回调直接返回。

### 7. 错误写法 vs 正确写法

#### 错误写法

- 在 `AIClient` 里直接拼装 DeepSeek 请求体，导致 Provider 抽象失效。

#### 正确写法

- `AIClient` 只负责选择 Provider 和转发 `ChatRequest`，供应商差异全部留在 `IModelProvider` 实现里。

## 场景二：DeepSeek 请求/响应与环境配置契约

### 1. 作用范围 / 触发条件

- 触发：修改 `DeepSeekProvider` 请求字段、响应解析、鉴权头或 `Config` 的 Provider 配置格式。

### 2. 关键签名

- `DeepSeekProvider::DeepSeekProvider(ProviderConfig config, int timeout_ms = 30000)`
- `DeepSeekProvider::DeepSeekProvider(ProviderConfig config, int timeout_ms, HttpClient http_client)`
- `ChatResponse DeepSeekProvider::chat(const ChatRequest& request)`
- `void DeepSeekProvider::streamChat(const ChatRequest& request, StreamCallback callback)`
- `ProviderInfo DeepSeekProvider::info() const`

### 3. 契约

- `ProviderConfig::api_key` 必填；缺失时不能发请求。
- `base_url` 允许带或不带尾部 `/`，实现层会归一化。
- `default_model` 为空时使用实现内默认值；当前实现值来自 `DeepSeekProvider` 源码。
- 请求 JSON 至少包含 `model`、`messages`；存在 `tools` 时才追加 `tools`。
- 流式请求必须带 `stream: true`，并在 `stream_options` 中请求 `include_usage`。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| `api_key` 为空 | 抛 `std::invalid_argument` |
| 服务端返回非 2xx | 抛 `std::runtime_error`，优先拼接远端错误信息 |
| 返回体缺少关键字段 | 抛 `std::runtime_error` 或 JSON 异常 |
| `tools` 为空 | 请求体不写 `tools` 字段 |

### 5. Good / Base / Bad

- Good：鉴权头使用 `Bearer <api_key>`，请求体只包含当前请求需要的字段。
- Base：只传普通文本消息，不传温度和工具，也能生成合法请求。
- Bad：把 `api_key` 写进 JSON 请求体，或把尾部 `/` 未处理的 URL 直接拼接成双斜杠路径。

### 6. 必要测试

- 请求体测试：验证 `messages` 顺序、`tools` 条件写入、流式模式字段。
- 配置测试：验证 `.env` / JSON 装载后能正确进入 `ProviderConfig`。
- 联机测试：仅在 `DEEPSEEK_API_KEY` 存在时运行，缺失时 `GTEST_SKIP()`。

### 7. 错误写法 vs 正确写法

#### 错误写法

- 在多个位置分别拼接 `Authorization` 头和请求 URL。

#### 正确写法

- 统一由 `DeepSeekProvider` 的辅助函数构建头和 URL，避免配置归一化逻辑分散。

## 场景三：HTTP 传输与 SSE 流式解析契约

### 1. 作用范围 / 触发条件

- 触发：修改 `HttpClient`、`SSEParser` 或 `DeepSeekProvider::streamChat` 的缓冲与事件回调逻辑。

### 2. 关键签名

- `HttpResponse HttpClient::postJson(...)`
- `HttpResponse HttpClient::postJsonStream(..., HttpStreamCallback callback)`
- `virtual HttpResponse IHttpTransport::postJson(...) const`
- `virtual HttpResponse IHttpTransport::postJsonStream(...) const`
- `std::vector<StreamEvent> SSEParser::parseChunk(const std::string& chunk) const`
- `struct ToolCallDelta { std::size_t index; std::optional<std::string> id; std::optional<std::string> name; std::string arguments_delta; }`

### 3. 契约

- `HttpClient` 负责一次请求的一次传输，不向上层暴露 `cpr` 类型。
- `IHttpTransport` 是窄传输边界；默认实现使用 cpr，本地确定性测试可注入脚本实现。
- `postJsonStream()` 在网络回调阶段收集完整响应文本，并在回调内部异常时延后重抛。
- `DeepSeekProvider::streamChat()` 负责把原始字节流缓冲成完整 SSE 事件边界，再交给 `SSEParser`。
- `SSEParser` 接收“完整事件块”而非任意碎片，支持 `\r\n`、`\n`、`\r` 归一化。
- 遇到 `[DONE]` 返回 `Done` 事件；遇到错误对象或非法 JSON 返回 `Error` 事件。
- 工具调用增量必须由 `SSEParser` 从 Provider 的嵌套 JSON 转换为 `StreamEvent::tool_call_deltas`。每个分片保留非负 `index`、可选 `id`/`name` 和字符串 `arguments_delta`；上层不得重新解析供应商原始 JSON。
- `tool_calls` 中的非数组、非对象项、缺失/负数/溢出索引，或 ID、名称、参数片段的非字符串类型，必须返回 `Error` 事件；不得静默转换为文本或不安全容器下标。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| 传输层错误 | `HttpClient` 抛 `std::runtime_error` |
| 回调内部抛异常 | 请求结束后重新抛出原异常 |
| SSE 块中 `content` 为 `null` | 解析器忽略该增量，不产出文本事件 |
| SSE 数据非法 | 生成 `Error` 事件 |
| Tool Call 增量结构或字段类型非法 | 生成 `Error` 事件，错误消息为“流式工具调用增量格式无效” |
| 网络结束时仍有残余缓冲 | `DeepSeekProvider` 对尾块做最后一次解析 |

### 5. Good / Base / Bad

- Good：Provider 先按空行边界缓冲，再把完整事件交给 `SSEParser`；解析器将工具 JSON 收敛为结构化 `ToolCallDelta`。
- Base：只返回文本增量，未包含工具调用，也能持续回调。
- Bad：把每次底层回调收到的半截字符串直接交给 `SSEParser`，或把 `tool_calls.dump()` 作为未解析字符串交给 Agent。

### 6. 必要测试

- `tests/http/sse_parser_test.cpp` 必须覆盖：正常、`null` 内容、`Done`、错误对象，以及交错多 Tool Call 的 `index`、ID、名称和参数分片解析。
- 同一测试必须断言负数索引等非法工具增量返回 `Error`，不产生 `ToolCallDelta`。
- `HttpClient` 相关测试或后续补测需覆盖回调抛错重抛语义。
- Provider 流式测试需覆盖尾部残余缓冲刷新。

### 7. 错误写法 vs 正确写法

#### 错误写法

- 假设底层网络库每次都会按 SSE 事件边界回调。

#### 正确写法

- 先在 Provider 层维护 `pending_buffer`，只对完整事件调用解析器。

## 场景四：Tool Call 注册与单批执行契约

### 1. 作用范围 / 触发条件

- 触发：修改 `Tool`、`ToolRegistry`、`ToolExecutor`、`ToolExecutionResult` 或 `AIClient` 的工具公开入口。
- 目标：让 SDK 提供可组合的模型工具能力，同时防止工具执行接口隐式演变成 Agent Loop。

### 2. 关键签名

- `void ToolRegistry::registerTool(const Tool& tool, ToolHandler handler)`
- `std::vector<Tool> ToolRegistry::listTools() const`
- `ToolResult ToolRegistry::execute(const std::string& name, const nlohmann::json& arguments)`
- `std::vector<ToolExecutionResult> ToolExecutor::executeAll(const std::vector<ToolCall>& calls)`
- `std::vector<ToolExecutionResult> AIClient::executeToolCalls(const std::vector<ToolCall>& calls)`
- `std::vector<ToolExecutionResult> AIClient::executeToolCalls(const std::vector<ToolCall>& calls, TraceSession& trace_session)`
- `Message ToolExecutionResult::toToolMessage() const`

### 3. 契约

- `Tool::name` 必须非空，`ToolHandler` 必须可调用，`Tool::parameters` 顶层必须是 JSON 对象。
- 同名 `registerTool` 替换最新定义和处理函数，但 `listTools()` 保持首次注册顺序且不产生重复项。
- `ChatRequest::tools` 由调用方显式赋值，例如 `request.tools = client.tools().listTools()`；注册工具不会隐式修改请求。
- `executeToolCalls()` 只同步、串行执行传入的一批 `ToolCall`，返回数量和顺序与输入一致。
- `ToolExecutionResult::toToolMessage()` 使用原 `call.id` 生成 `tool_call_id`；成功内容是 `result.data.dump()`，失败内容是 `result.error_message`。
- `AIClient` 不追加消息历史、不自动再次调用 `chat()`、不判断是否继续循环；这些决策属于上层应用或 Agent。
- `ToolRegistry` 当前不提供内部同步，并发注册或执行必须由调用方互斥。
- Trace 重载保持相同批次结果契约；步骤层级、默认数据和线程安全边界引用 [Trace 契约](./trace-contracts.md)。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| 工具名称为空 | `registerTool` 抛 `std::invalid_argument` |
| 处理函数为空 | `registerTool` 抛 `std::invalid_argument`，消息含工具名 |
| `parameters` 顶层不是对象 | `registerTool` 抛 `std::invalid_argument`，消息含工具名 |
| `getTool` 查询未知名称 | 抛 `std::out_of_range` |
| `execute` 收到未知名称 | 返回 `success == false` 的 `ToolResult`，不抛异常 |
| 工具处理函数抛标准异常 | 返回失败 `ToolResult`，消息含工具名和 `what()` |
| 单批中某个工具失败 | 保留该位置的失败结果，继续执行后续调用 |
| `executeToolCalls({})` | 返回空结果，不发起网络请求 |

### 5. Good / Base / Bad

- Good：调用方注册工具，把 `listTools()` 显式放入请求；收到响应后调用 `executeToolCalls(response.tool_calls)`，再自行决定是否调用 `toToolMessage()` 和下一次 `chat()`。
- Base：只注册并离线执行本地工具，不提供 API Key，也能通过 `AIClient::executeToolCalls()` 得到结果。
- Bad：在 `AIClient::executeToolCalls()` 内自动追加 assistant/tool 消息并循环调用 Provider，导致 SDK 隐式承担 Agent 决策职责。

### 6. 必要测试

- `tests/tool/tool_registry_test.cpp`：断言注册顺序、同名替换、非法定义、未知工具和处理函数异常。
- `tests/tool/tool_executor_test.cpp`：断言批量结果数量与顺序、单个失败后继续执行、成功和失败消息都绑定原 `tool_call_id`。
- `tests/smoke/ai_sdk_smoke_test.cpp`：断言 `AIClient::executeToolCalls()` 无 API Key 时仍能离线执行，且不会触发网络请求。
- `examples/04_register_tool`：离线验证注册、列举和单批执行。
- `examples/05_tool_call`：联机验证 DeepSeek 返回 Tool Call、SDK 执行工具、示例应用显式发起一次补充请求。

### 7. 错误写法 vs 正确写法

#### 错误写法

```cpp
// 错误：SDK 门面隐藏循环、历史修改和是否继续的决策。
ChatResponse AIClient::chatWithTools(ChatRequest request) {
    while(true) {
        // 隐式调用模型并执行工具……
    }
}
```

#### 正确写法

```cpp
request.tools = client.tools().listTools();
const ChatResponse response = client.chat(request);
const std::vector<ToolExecutionResult> results =
    client.executeToolCalls(response.tool_calls);

// 是否转换结果消息并发起下一次 chat，由上层调用方显式决定。
```
