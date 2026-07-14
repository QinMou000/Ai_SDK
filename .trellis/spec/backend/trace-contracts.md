# Trace 契约

## 场景：显式 Trace 跨层记录

### 1. 作用范围 / 触发条件

- 触发：修改 `include/trace/`、`src/trace/`、`AIClient` 的 Trace 重载，或 Provider、HTTP、SSE、工具执行中的链路记录。
- 目标：保证 Trace 始终是调用方显式持有的结构化旁路数据，不演变为日志文本、全局上下文或自动 Agent Loop。
- 约束：Trace 接入不得改变无 Trace 路径的返回值、异常传播、流式回调和工具批次结果。

### 2. 关键签名

- `TraceSession AIClient::startTrace(TraceOptions options = {}) const`
- `ChatResponse AIClient::chat(const ChatRequest&, TraceSession&) const`
- `void AIClient::streamChat(const ChatRequest&, StreamCallback, TraceSession&) const`
- `std::vector<ToolExecutionResult> AIClient::executeToolCalls(const std::vector<ToolCall>&, TraceSession&)`
- `Trace TraceSession::snapshot() const`
- `nlohmann::json TraceSession::toJson() const`
- `TraceStepScope TraceRecorder::startStep(TraceStepType, std::string_view parent_step_id = {}) const noexcept`
- `void TraceStepScope::setAttribute(TraceAttributeKey, Value&&) noexcept`
- `void TraceStepScope::setSanitizedDetail(TraceDetailSlot, const TraceDetailContext&, const nlohmann::json&) noexcept`
- `void TraceStepScope::fail(TraceFailure) noexcept`

脱敏器上下文与回调契约如下：

```cpp
struct TraceDetailContext {
    TraceDetailKind kind;
    std::string_view operation_name;
};

using TraceDetailSanitizer = std::function<nlohmann::json(
    const TraceDetailContext& context,
    const nlohmann::json& raw_value)>;
```

`operation_name` 只在脱敏器同步调用期间有效。模型请求与响应使用 Provider 名称，工具参数与结果使用工具名称；调用方如需跨调用保存名称，必须自行复制。

### 3. 数据与行为契约

#### 会话、顺序与层级

- `Config::enable_trace` 是 `AIClient` 的唯一总开关；关闭时 `startTrace()` 返回禁用句柄，不生成 ID、时间戳或共享状态。
- 当前客户端关闭总开关时，即使传入其他客户端创建的有效会话，也必须直接走无 Trace 路径。
- `TraceSession` 可复制，副本共享同一内部状态；步骤追加、完成、快照和 JSON 导出必须线程安全。
- 步骤序号与步骤追加必须在同一互斥区完成；快照按开始序号排序，不依赖完成顺序。
- `snapshot()` 按值返回一致时刻的视图，禁止返回内部容器引用。
- 同一会话可跨多次公开操作复用；每次 `chat`、`streamChat` 或工具批次都是新根步骤，不根据“最近一步”推断父节点。
- 父步骤无法创建时，后续接入层不得把本应属于它的子步骤降级为新根步骤。

```text
model_request
└─ provider_request
   ├─ http_request
   └─ sse_stream

tool_batch
└─ tool_execution
```

- 同步模型请求没有 `sse_stream`。
- 流式请求的 HTTP 与 SSE 是 Provider 下的兄弟步骤，因为网络返回后仍可能冲刷残余 SSE 缓冲。
- `SSEParser` 保持纯解析；Provider 只记录一次流级汇总，禁止逐分块或逐 Token 创建步骤。
- 工具批次和每个工具结果保持输入顺序；单工具失败标记对应子步骤和批次错误，但仍返回完整批次结果。

#### JSON 步骤结构

每个导出步骤固定包含以下字段：

- `step_id`、`parent_step_id`、`sequence`、`type`、`status`。
- `started_at`、`duration_ms`、`attributes`、`details`。
- `error_code`、`error_summary`。

成功步骤的 `error_code` 固定为 `none`，失败步骤只能使用 `TraceFailure` 的集中映射。`error_summary` 只能来自同一枚举的固定安全中文摘要，禁止接收任意字符串、`exception.what()` 或远端错误正文。

#### 默认属性白名单

接入层只能通过 `TraceAttributeKey` 写入以下类别，禁止自由字符串键：

- Provider 与模型：Provider、Model、流式标识、回调是否存在、消息数量、工具定义数量和工具调用数量。
- Token 用量：提示、补全和总 Token 数量。
- HTTP：方法、超时、状态码、响应字节数和分块数量。
- SSE：文本增量数量、工具增量数量、`Done` 数量、错误事件数量和总事件数量。
- 工具：工具名称、输入序号、批次数量、成功数、失败数和单工具成功标识。
- 步骤固有字段：固定步骤类型、状态、开始时间、耗时、错误码和安全错误摘要。

白名单只允许保存“数量”和固定元数据。SSE 文本增量、工具增量、远端错误事件正文及工具数据原文都不属于白名单。

默认禁止写入：

- API Key、完整 `Authorization` 头、URL、请求头和请求体。
- 用户消息、模型响应正文、`raw_response`、SSE delta 与远端错误正文。
- `ToolCall::raw_arguments`、工具参数、工具结果和工具错误原文。
- `exception.what()`；现有异常可能包含 URL、远端正文或扩展处理函数文本。

#### 详情槽位与脱敏原始输入

- `TraceOptions::detail_sanitizer` 为空时，`details` 保持空对象。
- 详情顶层槽位只能由 `TraceDetailSlot` 选择：`request`、`response`、`arguments`、`result`。
- 每个已处理槽位固定为 `{"status": <状态>, "value": <对象>}`。
- 状态只允许 `recorded`、`rejected`、`sanitizer_failed`。
- 脱敏器返回顶层 JSON 对象时使用 `recorded` 并保存返回对象；返回数组、标量或 `null` 时使用 `rejected`；抛任意异常时使用 `sanitizer_failed`。
- `rejected` 和 `sanitizer_failed` 的 `value` 固定为空对象，不保存异常文本或原始输入。
- 脱敏器必须在 Trace 状态锁外调用；调用结束后必须重新检查步骤仍处于 `Running`，避免重入完成步骤后继续写详情。
- TraceSession 的线程安全不延伸到调用方闭包；并发使用同一会话时，脱敏器自身同步由调用方负责。

四类原始输入协议如下：

| `TraceDetailKind` | 槽位 | `raw_value` 协议 | `operation_name` |
| --- | --- | --- | --- |
| `ModelRequest` | `request` | `chatRequestToJson(request)` 的完整对象，可能包含消息、工具定义和模型参数 | 当前 Provider 名称 |
| `ModelResponse` | `response` | `chatResponseToJson(response)` 的完整对象，可能包含正文、工具调用、用量和 `raw_response` | 当前 Provider 名称 |
| `ToolArguments` | `arguments` | 解析后的 `ToolCall::arguments`；禁止改传 `raw_arguments` | 当前工具名称 |
| `ToolResult` | `result` | 成功时为 `{"success": true, "data": ...}`；失败时为 `{"success": false, "error_message": ...}` | 当前工具名称 |

原始输入只允许作为脱敏器的同步参数，不得先复制进 Trace 状态。调用方必须按 `kind` 与 `operation_name` 建立显式字段白名单，不能直接返回 `raw_value`。

#### 生命周期、时钟与错误隔离

- 步骤开始时立即写入 `Running`，使用 `system_clock` 生成 UTC 开始时间。
- 耗时只使用 `steady_clock` 计算，并钳制为非负毫秒。
- 正常路径必须显式调用 `succeed()`；已知失败必须调用 `fail(TraceFailure)`。
- RAII 步骤作用域析构时若尚未完成，记录为 `Error`、`step_abandoned` 和固定摘要“步骤未正常结束”。
- Trace 创建以外的内部记录方法必须 `noexcept` 并收敛自身异常，不能覆盖业务异常或返回值。
- SSE `Error` 事件只把 SSE 子步骤标记错误，继续沿用事件回调契约；用户回调抛错则原异常继续传播并关闭所有在途步骤。

### 4. 校验与错误矩阵

| 条件 | Trace 行为 | 业务行为 |
| --- | --- | --- |
| `enable_trace == false` | 不创建步骤或标识 | 完全走原无 Trace 路径 |
| 脱敏器为空 | 对应详情槽位不出现 | 请求、回调和工具结果不变 |
| 脱敏器返回对象 | 写入 `recorded` 与返回对象 | 继续原流程 |
| 脱敏器返回非对象 | 写入 `rejected` 与空对象 | 继续原流程 |
| 脱敏器抛异常 | 写入 `sanitizer_failed` 与空对象 | 吞掉脱敏异常并继续原流程 |
| Trace 内部字段构造或更新失败 | 丢弃当前字段；已创建但无法继续包装的步骤使用 `trace_recording_failed` 安全结束 | 不改变返回值或覆盖业务异常 |
| 业务层已知失败 | 使用固定 `TraceFailure` 机器码和摘要结束步骤 | 沿用原异常或失败结果契约 |
| 步骤离开作用域但未完成 | 记录 `step_abandoned` | 不从析构函数抛异常 |
| SSE 收到错误事件 | SSE 步骤记录固定错误码 | 仍按既有事件协议回调 |

### 5. 良好 / 基础 / 错误用例

- 良好用例：同一显式会话串联模型请求、工具批次和后续模型请求；每层传递明确父步骤 ID，详情按操作名称分别脱敏。
- 基础用例：只启用默认元数据，不设置脱敏器；导出包含稳定步骤字段与数量，不包含任何业务原文。
- 错误用例：把 SSE delta、用户消息、工具参数或 `exception.what()` 直接写入 `attributes`、`details` 或 `error_summary`。

### 6. 必要测试

- 总开关关闭、有效会话不可绕过关闭配置，且无 Trace 重载保持原行为。
- ID 格式与唯一性、明确父子关系、父步骤失败时不产生孤立根步骤、稳定 JSON 字段。
- JSON 每步包含 `error_code`；成功值为 `none`，失败值与 `TraceFailure` 映射一致。
- RAII 兜底、并发追加、并发快照、序号完整性与脱敏器重入完成步骤。
- 模型请求、Provider、HTTP、SSE、工具批次和单工具步骤。
- 非 2xx、传输失败、SSE 错误、缺少 `Done`、回调异常、工具失败和空工具批次。
- 四类脱敏原始输入必须分别覆盖正常 `recorded` 路径，并断言 `raw_value` 形状与 `operation_name`：模型使用 Provider 名，工具使用工具名；仅覆盖脱敏器异常不能替代正常协议断言。
- `recorded`、`rejected`、`sanitizer_failed` 三种详情状态及其 `value` 约束。
- 导出文本不包含测试植入的密钥、URL、消息、正文、参数、结果和异常哨兵。

优先通过 `IHttpTransport` 注入本地脚本传输完成确定性测试，不把真实外网或可跳过在线测试作为 Trace 验收依据。

### 7. 错误写法 vs 正确写法

#### 错误写法

```cpp
// 错误：自由字符串键和原始值会绕过默认白名单。
step.setAttribute("response_body", response.text);
step.fail(exception.what());

options.detail_sanitizer = [](const auto&, const nlohmann::json& raw) {
    return raw;
};
```

#### 正确写法

```cpp
// 正确：默认路径只写强类型白名单元数据和固定失败枚举。
step.setAttribute(TraceAttributeKey::ResponseBytes, response.text.size());
step.fail(TraceFailure::HttpTransportFailed);

// 正确：按类别和操作名称只返回业务允许的摘要字段。
options.detail_sanitizer = [](const TraceDetailContext& context,
                              const nlohmann::json& raw) {
    if(context.kind == TraceDetailKind::ToolArguments &&
       context.operation_name == "calculator") {
        return nlohmann::json{{"argument_field_count", raw.size()}};
    }
    return nlohmann::json::object();
};
```
