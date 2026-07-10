# SDK 契约

## 场景一：`AIClient` 到 Provider 的聊天契约

### 1. 作用范围 / 触发条件

- 触发：修改 `AIClient`、`IModelProvider`、`DeepSeekProvider`，或新增其他 Provider。
- 目标：保证同步聊天与流式聊天在公开 API、配置读取和错误传播上保持一致。

### 2. 关键签名

- `ChatResponse AIClient::chat(const ChatRequest& request)`
- `void AIClient::streamChat(const ChatRequest& request, StreamCallback callback)`
- `virtual ChatResponse IModelProvider::chat(const ChatRequest& request) = 0`
- `virtual void IModelProvider::streamChat(const ChatRequest& request, StreamCallback callback) = 0`

### 3. 契约

- `AIClient` 构造时接收完整 `Config`，并按 `default_provider` 选择 Provider。
- `Config::providers` 必须包含目标 Provider 的配置；当前仅支持键名 `deepseek`。
- `ChatRequest` 必须保留消息顺序，`messages` 为空时依然属于调用方错误输入。
- `chat()` 返回完整 `ChatResponse`；`streamChat()` 通过回调逐步返回文本增量或工具增量。
- `streamChat()` 允许空回调时直接返回，不主动发请求。

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

- `DeepSeekProvider::DeepSeekProvider(const ProviderConfig& config)`
- `nlohmann::json DeepSeekProvider::buildRequestJson(const ChatRequest& request, bool stream) const`
- `std::vector<std::string> DeepSeekProvider::buildHeaders() const`

### 3. 契约

- `ProviderConfig::api_key` 必填；缺失时不能发请求。
- `base_url` 允许带或不带尾部 `/`，实现层会归一化。
- `default_model` 为空时使用实现内默认值；当前实现值来自 `DeepSeekProvider` 源码。
- 请求 JSON 至少包含 `model`、`messages`；存在 `tools` 时才追加 `tools`。
- 流式请求必须带 `stream: true`，并在 `stream_options` 中请求 `include_usage`。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| `api_key` 为空 | 抛 `std::runtime_error` |
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

- `HttpResponse HttpClient::post(const HttpRequest& request)`
- `void HttpClient::postStream(const HttpRequest& request, StreamHandler handler)`
- `std::vector<SSEEvent> SSEParser::parseChunk(std::string_view chunk)`

### 3. 契约

- `HttpClient` 负责一次请求的一次传输，不向上层暴露 `cpr` 类型。
- `postStream()` 在网络回调阶段收集完整响应文本，并在回调内部异常时延后重抛。
- `DeepSeekProvider::streamChat()` 负责把原始字节流缓冲成完整 SSE 事件边界，再交给 `SSEParser`。
- `SSEParser` 接收“完整事件块”而非任意碎片，支持 `\r\n`、`\n`、`\r` 归一化。
- 遇到 `[DONE]` 返回 `Done` 事件；遇到错误对象或非法 JSON 返回 `Error` 事件。

### 4. 校验与错误矩阵

| 条件 | 行为 |
| --- | --- |
| 传输层错误 | `HttpClient` 抛 `std::runtime_error` |
| 回调内部抛异常 | 请求结束后重新抛出原异常 |
| SSE 块中 `content` 为 `null` | 解析器忽略该增量，不产出文本事件 |
| SSE 数据非法 | 生成 `Error` 事件 |
| 网络结束时仍有残余缓冲 | `DeepSeekProvider` 对尾块做最后一次解析 |

### 5. Good / Base / Bad

- Good：Provider 先按空行边界缓冲，再把完整事件交给 `SSEParser`。
- Base：只返回文本增量，未包含工具调用，也能持续回调。
- Bad：把每次底层回调收到的半截字符串直接交给 `SSEParser`，导致 JSON 被截断。

### 6. 必要测试

- `tests/http/sse_parser_test.cpp` 风格的正常、`null` 内容、`Done`、错误对象测试。
- `HttpClient` 相关测试或后续补测需覆盖回调抛错重抛语义。
- Provider 流式测试需覆盖尾部残余缓冲刷新。

### 7. 错误写法 vs 正确写法

#### 错误写法

- 假设底层网络库每次都会按 SSE 事件边界回调。

#### 正确写法

- 先在 Provider 层维护 `pending_buffer`，只对完整事件调用解析器。
