#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace aiSDK {

// 本文件只定义公开快照值和稳定 JSON 映射声明。
// 线程、随机数、锁与 RAII 生命周期均留在 TraceRecorder 实现中。
// 调用方可以保存这些值，但不应手工伪造它们驱动 SDK 业务分支。
// TraceStepType 使用固定枚举表达 SDK 已知的链路边界，
// 避免调用方依赖容易漂移的自由文本类型。
// 枚举只描述职责层，不编码供应商名称或业务动作。
// 同一类型可在一个会话中出现多次，顺序由 sequence 表达。
enum class TraceStepType { ModelRequest, ProviderRequest, HttpRequest, SseStream, ToolBatch, ToolExecution };

// TraceStepStatus 描述步骤当前状态。
// 快照可能在步骤尚未结束时读取，因此 Running 也是稳定公开状态。
// Success 表示该层契约正常完成，Error 不必等同于抛出异常。
// 例如 SSE Error 事件会标记流步骤错误，但仍按事件协议返回。
enum class TraceStepStatus { Running, Success, Error };

// TraceDetailKind 告诉调用方脱敏器当前收到的数据类别。
// SDK 只会保存脱敏器返回的对象，不会自动保存传入的原始值。
// 分类用于让一个脱敏器按数据来源采用不同白名单策略。
// 它不表示详情已经保存，实际结果仍由 TraceOptions 决定。
enum class TraceDetailKind { ModelRequest, ModelResponse, ToolArguments, ToolResult };

// TraceAttributeKey 是允许进入默认 attributes 的完整白名单。
// 接入层只能选择枚举值，不能用任意字符串把原始业务数据写入 Trace。
enum class TraceAttributeKey {
    Provider,
    Model,
    Stream,
    MessageCount,
    ToolDefinitionCount,
    ToolCallCount,
    PromptTokens,
    CompletionTokens,
    TotalTokens,
    CallbackPresent,
    HttpMethod,
    TimeoutMs,
    HttpStatusCode,
    ResponseBytes,
    ChunkCount,
    DeltaCount,
    ToolCallDeltaCount,
    DoneCount,
    ErrorCount,
    EventCount,
    ToolCount,
    SuccessCount,
    FailureCount,
    ToolName,
    ToolIndex,
    Success
};

// TraceDetailSlot 固定脱敏详情在步骤中的位置，
// 防止调用方返回的对象覆盖步骤保留字段或创建任意顶层键。
enum class TraceDetailSlot { Request, Response, Arguments, Result };

// 每个详情槽位都带独立状态，调用方无需解析诊断文案。
// 失败或拒绝状态下 value 固定为空对象，原始值不会进入 Trace。
enum class TraceDetailStatus { Recorded, Rejected, SanitizerFailed };

// TraceFailure 既提供稳定机器码，也集中生成固定中文安全摘要。
// 失败枚举不携带 exception.what()、远端正文或任何调用方输入。
// 各层只选择最接近自身职责的枚举，不负责拼接下层错误原因。
// 同一次异常可让多个在途步骤分别失败，以保留故障传播层级。
// HTTP 状态失败、网络失败和下游回调失败必须保持独立分类。
// SSE Error 事件与 C++ 异常不同，它只描述协议内观察到的事实。
// 工具业务失败不会中断批次，因此同时存在子步骤与批次失败码。
// None 只用于成功步骤，接入层应通过 succeed() 写入该状态。
enum class TraceFailure {
    None,
    StepAbandoned,
    ScopeReplaced,
    TraceRecordingFailed,
    ModelRequestFailed,
    StreamModelRequestFailed,
    ProviderRequestFailed,
    ProviderStreamFailed,
    HttpStatusFailed,
    HttpTransportFailed,
    HttpStreamFailed,
    StreamCallbackFailed,
    SseErrorEvent,
    SseIncomplete,
    SseProcessingFailed,
    ToolExecutionFailed,
    ToolBatchException,
    ToolBatchPartialFailure
};

// TraceStep 是一次可观测操作的只读快照值对象。
// attributes 仅保存 SDK 固定白名单元数据，details 仅保存调用方脱敏后的对象。
// started_at 用于跨步骤对照墙钟时间，duration_ms 来自单调时钟。
struct TraceStep {
    std::string step_id;
    std::optional<std::string> parent_step_id;
    std::uint64_t sequence = 0;
    TraceStepType type = TraceStepType::ModelRequest;
    TraceStepStatus status = TraceStepStatus::Running;
    std::string started_at;
    long long duration_ms = 0;
    nlohmann::json attributes = nlohmann::json::object();
    nlohmann::json details = nlohmann::json::object();
    TraceFailure failure = TraceFailure::None;
    std::string error_summary;
};

// Trace 保存同一显式调用链的标识和有序步骤。
// steps 始终按开始序号排序，不依赖并发任务的完成顺序。
struct Trace {
    std::string trace_id;
    std::vector<TraceStep> steps;
};

// 集中的字符串映射是 JSON 契约的唯一来源，
// 新增枚举值时必须同步更新实现和序列化测试。
const char* traceStepTypeToString(TraceStepType type) noexcept;
const char* traceStepStatusToString(TraceStepStatus status) noexcept;
const char* traceDetailKindToString(TraceDetailKind kind) noexcept;
const char* traceAttributeKeyToString(TraceAttributeKey key) noexcept;
const char* traceDetailSlotToString(TraceDetailSlot slot) noexcept;
const char* traceDetailStatusToString(TraceDetailStatus status) noexcept;
const char* traceFailureToCode(TraceFailure failure) noexcept;
const char* traceFailureToSummary(TraceFailure failure) noexcept;

// JSON 导出只读取快照值，不持有 TraceSession 内部锁。
// 所有字段固定存在，便于调用方稳定消费和持久化。
nlohmann::json traceStepToJson(const TraceStep& step);
nlohmann::json traceToJson(const Trace& trace);

}  // namespace aiSDK
