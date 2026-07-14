# Trace 上下文与公开 API 形态研究

## 研究目标

为 `ai_sdk` 的完整显式 Trace 链路选择上下文所有权、标识和传递方式。目标是借鉴成熟追踪模型，但不在 MVP 中引入 OpenTelemetry 或远程追踪依赖。

## 参考资料

- OpenTelemetry Traces 概念：https://opentelemetry.io/docs/concepts/signals/traces/
- OpenTelemetry C++ 手工埋点：https://opentelemetry.io/docs/languages/cpp/instrumentation/
- W3C Trace Context：https://www.w3.org/TR/trace-context/

## 可复用惯例

### 1. Trace 与 Span 分层

- OpenTelemetry 使用一个 `trace_id` 关联整条逻辑链，每个有开始/结束边界的操作使用独立 Span。
- Span 之间通过父子关系表达模型请求、HTTP 请求、工具执行等嵌套操作。
- Span 使用结构化属性、事件和状态承载事实；日志不是 Trace 的数据源。

映射到本仓库：`Trace` 应表示完整显式调用链，`TraceStep` 应演进为有独立步骤标识、父步骤标识、开始时间、耗时、状态和受控属性的 Span 等价物。

### 2. 显式上下文与作用域上下文

- OpenTelemetry C++ 同时支持显式传递 Context，以及通过 RAII `Scope` 设置当前活动 Span。
- 活动 Scope 适合同一同步调用栈内自动建立父子关系；跨多个公开 API 的业务链路仍需要可传播的上下文。
- 本仓库的调用方会显式执行 `chat`、`executeToolCalls` 和后续 `chat`，因此显式句柄比客户端“最近一次 Trace”或线程局部隐式状态更清楚。

映射到本仓库：公开 API 使用显式 `TraceSession`/`TraceContext`；内部可以用小型 RAII Span 作用域保证成功、异常路径都能结束计时，但不把线程局部上下文作为公开契约。

### 3. 标识与隐私

- W3C 使用 16 字节 `trace-id` 和 8 字节 `parent-id`，以小写十六进制编码；全零值无效。
- 标识只用于关联，不应编码 IP、用户标识或其他敏感信息；随机源不能以可识别用户的信息作为种子。
- W3C HTTP 头传播解决跨服务问题，本仓库 MVP 只需采用稳定标识语义，不需要立即写入远端请求头。

映射到本仓库：生成 32 字符小写十六进制 `trace_id` 与 16 字符步骤 ID；MVP 不发送 `traceparent`，但数据模型为未来传播保留兼容空间。

## 可选方案

### 方案 A：显式 `TraceSession` 句柄（推荐）

示意：

```cpp
auto trace = client.startTrace();
client.chat(request, trace);
client.executeToolCalls(calls, trace);
client.chat(follow_up, trace);
const Trace snapshot = trace.snapshot();
```

- 优势：调用链所有权明确；同一上下文可跨公开 API 复用；并发链路互不覆盖；调用方可以决定何时读取或导出。
- 劣势：公开 API 需要增加 Trace 参数；句柄内部共享状态的复制、线程安全和生命周期必须明确。
- 适配：最符合用户选择的“完整显式链路”，也不让 `AIClient` 隐式承担 Agent Loop。

### 方案 B：每次调用传入可空 `TraceRecorder*`

示意：

```cpp
TraceRecorder trace(...);
client.chat(request, &trace);
client.executeToolCalls(calls, &trace);
```

- 优势：实现直接；关闭 Trace 时传空指针即可；不需要额外句柄类型。
- 劣势：裸指针生命周期容易误用；调用方需要手工初始化 `trace_id`、Provider、Model；难以集中控制只读快照和结束语义。
- 适配：可做内部接口，但不适合作为长期公开 API。

### 方案 C：RAII `TraceScope` / 当前活动 Trace

示意：

```cpp
auto scope = client.startTraceScope();
client.chat(request);
client.executeToolCalls(calls);
```

- 优势：调用点最简洁；同步调用栈内可自动关联。
- 劣势：需要线程局部或客户端可变“当前 Trace”；跨线程、异步和同一客户端并发时语义复杂；容易形成隐藏状态。
- 适配：与已确认的“显式上下文”和无隐式全局状态约束冲突，不建议作为 MVP 公开契约。

## 推荐结论

采用方案 A：公开 `TraceSession` 句柄，调用方显式把同一个会话传给每次 SDK 操作。内部可以使用 RAII 步骤守卫保证计时和异常收敛，但不暴露或依赖全局活动 Trace。标识格式借鉴 W3C，暂不实现 HTTP 传播。

## 风险与验证重点

- 并发：同一 `TraceSession` 是否允许多线程追加必须明确；若允许，需要锁并保证快照一致性。
- 生命周期：SDK 不得保存调用方栈对象的悬空引用；句柄应使用明确的共享所有权或禁止跨对象复制。
- 关闭开关：`enable_trace == false` 时必须避免生成 ID、分配步骤和获取高精度时间。
- 异常：每个步骤必须通过 RAII 或等价机制在异常路径记录失败和耗时后继续透传原异常。
- 隐私：属性采用白名单，不接收或自动序列化完整请求体、鉴权头与工具原始参数。
