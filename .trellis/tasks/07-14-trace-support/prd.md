# 实现 Trace 链路追踪功能

## Goal

在不污染 SDK 正常输出、不泄露密钥或用户敏感内容的前提下，为模型请求与工具执行提供可关闭、可测试、可导出的内存 Trace，帮助调用方定位 Provider、HTTP、SSE 与 Tool Call 链路中的耗时和失败位置。

## What I already know

- 用户希望在完成 HTTP 流式注释提交后开始实现 Trace 功能。
- `include/trace/Trace.h` 已预留 `Trace`、`TraceStep` 数据结构，`include/trace/TraceRecorder.h` 已预留内存步骤累积器，但当前没有对应 `src/trace/` 实现、测试或 CMake 接入。
- `Config::enable_trace` 已存在，JSON 读写和配置测试已经覆盖该开关。
- `spdlog` 已由主库链接，但生产代码尚未形成统一 logger；项目日志规范要求默认静默、统一开关、禁止输出完整鉴权头/API Key/用户消息，并避免每个 token 一条日志。
- 现有 `AIClient` 是公开门面，聊天请求委托给 Provider；`DeepSeekProvider` 拥有供应商协议，`HttpClient` 只拥有传输语义，`ToolExecutor` 只执行调用方显式传入的一批工具调用。
- 项目原始 PRD 期望 Trace 能覆盖请求开始/结束、Provider、Model、HTTP 状态、耗时、Tool Call、错误，并支持 `trace_id`、JSON 导出和关闭 Trace。

## Constraints

- MVP 先采用进程内 Trace，不引入 SQLite、OpenTelemetry 或新的第三方依赖。
- Trace 是 SDK 的结构化数据能力，日志仅作为可选消费方式，不用日志文本充当 Trace 数据源。
- Trace 公开类型继续放在 `include/trace/`，实现放在 `src/trace/`，测试形成独立职责后放在 `tests/trace/`。
- 一个 Trace 覆盖由调用方显式串联的完整链路，包括模型请求、Tool Call 执行和后续模型请求。

## Resolved Preferences

- 链路范围：一个 Trace 覆盖调用方显式串联的模型请求、Tool Call 执行和后续模型请求。
- 公开入口：使用 `AIClient::startTrace()` 创建可显式传递的 `TraceSession`。
- 数据策略：默认元数据白名单，调用方可通过 `TraceOptions` 提供详情脱敏器。
- 并发语义：`TraceSession` 是可复制的线程安全共享句柄，同一会话允许多线程追加步骤和读取快照。

## Requirements

- 复用 `Config::enable_trace` 作为统一总开关，关闭时不生成 Trace，也不产生额外标准输出。
- Trace 数据结构不得包含完整 API Key、`Authorization` 头或默认明文用户消息。
- Trace 默认只记录元数据白名单，包括 Provider、Model、步骤类型、状态、HTTP 状态码、耗时、工具名称、数量和安全错误分类。
- `AIClient::startTrace(TraceOptions)` 允许调用方提供可选详情脱敏器；只有脱敏器显式返回的结构化字段可以进入 Trace。
- SDK 不自动保存原始消息、完整请求体、鉴权头、工具参数或工具结果；详情脱敏器抛错时忽略该详情并保留安全诊断标记，不能改变主调用结果或异常。
- 记录结构必须能表达步骤类型、状态、耗时和错误摘要，并可稳定导出 JSON。
- 同步聊天与流式聊天必须遵循一致的 Trace 生命周期和错误收敛规则。
- `AIClient::startTrace()` 创建显式 `TraceSession`；调用方把同一个会话传给每次需要关联的公开 SDK 操作。
- `TraceSession` 提供只读快照和 JSON 导出，调用方拥有会话生命周期，`AIClient` 不保存“最近一次 Trace”。
- `TraceSession` 的副本共享同一内部状态；步骤追加、状态更新和快照读取必须线程安全，不得丢失并发步骤或产生数据竞争。
- 每个步骤在开始时获得单调递增序号，并发快照和 JSON 导出按该序号稳定排序，不依赖线程调度完成顺序。
- 同一个显式 Trace 上下文必须能够跨 `chat` / `streamChat`、`executeToolCalls` 和后续模型请求复用，并保持步骤顺序。
- SDK 只记录调用方显式执行的步骤，不自动追加消息、不自动发起后续模型请求，也不承担 Agent Loop 决策。
- Trace 不能改变现有异常、返回值、流式回调和工具执行契约。

## Acceptance Criteria

- [x] `enable_trace == false` 时，现有聊天与工具执行行为保持不变且不产生 Trace。
- [x] 开启 Trace 后，每次纳入范围的操作都有非空且唯一的 `trace_id`。
- [x] 调用方复用同一个显式上下文时，模型请求、工具执行和后续模型请求共享同一个 `trace_id`，并按发生顺序形成完整步骤链。
- [x] `TraceSession` 的快照与 JSON 导出返回同一时刻的稳定数据，不要求调用方解析日志文本。
- [x] 多线程通过同一个 `TraceSession` 追加步骤并并发读取快照时，无数据竞争、无步骤丢失，排序结果稳定。
- [x] 成功、异常和流式错误路径都能形成状态明确、耗时非负的步骤记录。
- [x] Trace 可导出为结构稳定的 JSON，且默认不包含密钥、鉴权头和完整用户消息。
- [x] 未配置详情脱敏器时，模型消息、工具参数和工具结果均不会进入 Trace；配置后只导出脱敏器返回的字段。
- [x] 详情脱敏器抛出异常时，聊天、流式回调和工具执行仍保持原有结果或原有异常语义。
- [x] 正常流程、关闭开关、边界条件和错误恢复均有本地自动化测试。

## Definition of Done (team quality bar)

- 新增或更新 GTest 单元测试与跨层冒烟测试。
- `clang-format`、Windows Debug 构建和全部本地测试通过。
- README 或示例展示真实的 Trace 开启、读取与 JSON 导出方式。
- `.trellis/spec/` 同步记录最终 Trace 契约、错误边界与脱敏规则。
- 提供迁移说明；若公开预留类型被破坏性重构，明确旧接口移除方式。

## Technical Approach

- 将预留的 `Trace` / `TraceStep` 演进为 Span 风格结构：包含 `trace_id`、`step_id`、可选 `parent_step_id`、单调序号、步骤类型、状态、开始时间、耗时、白名单属性和安全错误摘要。
- 使用 `enum class` 表达步骤类型与状态，并提供集中 JSON 序列化，避免业务逻辑依赖自由字符串。
- `TraceSession` 是持有 `std::shared_ptr<State>` 的轻量可复制句柄；`State` 内含互斥锁、步骤集合、序号计数器和 `TraceOptions`。快照在持锁期间复制一致状态，JSON 在快照上生成。
- 内部使用 RAII 步骤守卫：开始步骤时立即分配序号并写入运行状态，成功或异常时更新最终状态与耗时；使用 `steady_clock` 计算耗时，使用 `system_clock` 生成可导出的时间戳。
- `AIClient::startTrace(TraceOptions)` 在 `enable_trace == true` 时生成随机 32 字符小写十六进制 `trace_id`；关闭时返回禁用会话，不生成 ID、不分配步骤，传入现有操作时保持空操作。
- 保留现有无 Trace 调用入口，并增加显式接收 `TraceSession&` 的重载；无 Trace 入口继续维持当前返回值、异常和流式回调契约。
- `AIClient` 记录公开操作边界，`DeepSeekProvider` 记录 Provider/Model 与协议结果，`HttpClient` 记录传输模式、超时和 HTTP 状态，`SSEParser` 只汇总事件数量与 Done/Error 事实，`ToolExecutor` 记录批次及单个工具步骤。
- 嵌套层之间显式传递父步骤 ID；跨多个调用方操作只保证共享 `trace_id` 和稳定顺序，不通过“最近一步”隐式推断父子关系。
- 详情脱敏器在互斥锁外执行，只允许返回结构化 JSON 对象；其异常被捕获并转换为安全诊断标记，原始内容不落入 Trace。
- 不引入 OpenTelemetry、SQLite 或新依赖；复用 C++17、`nlohmann::json` 和现有构建测试体系。MVP 不向远端请求注入 `traceparent`。

## Out of Scope (explicit)

- SQLite 或文件持久化。
- OpenTelemetry、远程 Collector、分布式跨进程上下文传播。
- Trace 可视化界面。
- 高频逐 token Trace 日志。
- 开启 Trace 后自动记录完整请求、消息、工具参数或工具结果原文。
- 自动 Agent Loop、隐式追加消息和隐式后续模型请求。
- 依赖线程局部变量或客户端“最近一次 Trace”的隐式全局上下文。

## Decision (ADR-lite)

### 链路与上下文

**Context**：现有 `chat`、`streamChat` 与 `executeToolCalls` 都是调用方显式触发的独立入口，但原始产品目标要求 Tool Call 能看到完整链路。

**Decision**：MVP 采用完整显式链路和公开 `TraceSession` 句柄。调用方通过 `AIClient::startTrace()` 创建会话，并将其显式传给 `chat` / `streamChat`、`executeToolCalls` 与后续模型请求；SDK 只把每次显式操作追加为该 Trace 的步骤。

**Consequences**：可以完整关联模型与工具步骤，调用方明确拥有生命周期，并避免客户端全局状态或隐藏 Agent Loop；代价是调用点需要显式携带会话，同一会话的并发语义需要继续确认。

### 数据与脱敏

**Context**：原始产品目标希望记录工具参数与结果，但项目日志规范禁止默认输出完整用户消息、鉴权信息和其他敏感原文。

**Decision**：默认只记录固定元数据白名单；调用方可在 `TraceOptions` 中显式提供详情脱敏器，SDK 只保存脱敏器返回的结构化结果。

**Consequences**：默认配置安全且可稳定测试，同时保留按业务场景增强诊断信息的能力；代价是自定义脱敏逻辑由调用方维护，其异常必须被 Trace 层隔离。

### 并发与所有权

**Context**：完整调用链未来可能包含并行模型请求或并行工具执行，且调用方可能复制 Trace 句柄交给不同线程。

**Decision**：`TraceSession` 采用可复制的共享所有权和内部互斥；并发步骤在开始时分配稳定序号，快照读取与状态更新使用同一同步边界。

**Consequences**：同一 Trace 可安全聚合多线程步骤，并为未来并行能力保留空间；代价是开启 Trace 后每次步骤变更都会产生锁开销，且步骤展示顺序定义为开始顺序而不是完成顺序。

## Technical Notes

- 依赖与集成点：`Config::enable_trace` → 调用方创建 Trace 上下文 → `AIClient` 把上下文传给 Provider/HTTP/SSE 与 ToolExecutor → 各层记录边界事实 → `TraceRecorder` 汇总 → 调用方读取或回调消费 → JSON 导出。
- 输入协议：`ChatRequest`、`StreamCallback`、`ToolCall`；输出协议：现有返回值/异常保持不变，Trace 通过独立公开接口暴露。
- 现有可复用模式：`AIClient` 门面负责跨模块入口；Provider 负责供应商字段；`HttpClient` 负责传输结果；`ToolExecutionResult` 收敛单批工具执行结果。
- 性能约束：关闭时应只有常量级分支成本；开启时避免复制完整请求/响应，锁只保护 Trace 状态，脱敏器和 JSON 序列化不在锁内执行，内存随步骤数增长而非随 token 数增长。
- 并发风险：不同 `TraceSession` 天然隔离；同一会话通过共享互斥保证写入与快照一致，但调用方脱敏器自身的线程安全仍由调用方负责并在文档中明确。
- 安全风险：任何 detail/error 字段都需明确脱敏来源，禁止直接序列化请求头或完整请求体。
- 已检查：`include/trace/Trace.h`、`include/trace/TraceRecorder.h`、`include/core/Config.h`、`src/core/Config.cpp`、`include/AIClient.h`、`src/AIClient.cpp`、`src/provider/DeepSeekProvider.cpp`、`src/http/HttpClient.cpp`、`src/http/SSEParser.cpp`、`src/tool/ToolExecutor.cpp`、相关测试与 `docs/PRD.md`。

## Research References

- [`research/trace-context-api.md`](research/trace-context-api.md)：对照 OpenTelemetry 与 W3C Trace Context，推荐公开显式 `TraceSession` 句柄，内部使用 RAII 步骤守卫，不引入新的追踪依赖。

## Implementation Plan

1. 重构 Trace 公共模型，完成 ID、状态、步骤、线程安全 `TraceSession`、RAII 守卫和 JSON 序列化，并先补齐 Trace 单元测试。
2. 扩展 `AIClient`、Provider、HTTP/SSE 与 ToolExecutor 的显式 Trace 传递和分层埋点，保持无 Trace 调用契约不变。
3. 覆盖关闭开关、同步/流式成功与失败、工具调用、脱敏器异常、并发追加和一致快照测试。
4. 新增最小可运行 Trace 示例，更新 README、迁移说明和 `.trellis/spec/`，最后执行格式、Windows Debug 构建与全部本地测试。
