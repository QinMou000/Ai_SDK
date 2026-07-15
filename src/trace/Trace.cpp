#include "trace/Trace.h"

namespace aiSDK {

// 类型字符串属于对外 JSON 协议，不能直接依赖编译器枚举名称。
// 使用 switch 能让新增类型在编译告警和测试中显式暴露。
// 返回静态字符串不会分配内存，也适合在错误路径中调用。
// unknown 只作为防御性兜底，不应由当前合法枚举值产生。
// Model、Provider、HTTP 和 SSE 保持分层，便于定位耗时归属。
// ToolBatch 与 ToolExecution 分开，允许表达部分失败批次。
// 修改字符串会影响持久化消费者，必须同步迁移说明与测试。
const char* traceStepTypeToString(TraceStepType type) noexcept {
    switch(type) {
        case TraceStepType::ModelRequest:
            return "model_request";
        case TraceStepType::ProviderRequest:
            return "provider_request";
        case TraceStepType::HttpRequest:
            return "http_request";
        case TraceStepType::SseStream:
            return "sse_stream";
        case TraceStepType::ToolBatch:
            return "tool_batch";
        case TraceStepType::ToolExecution:
            return "tool_execution";
    }
    return "unknown";
}

// 状态映射与类型映射遵循同一稳定协议。
// Running 允许调用方观察尚未完成的并发步骤。
// Success 与 Error 表达本层契约结果，不改变业务控制流。
// 字符串使用小写下划线，便于跨语言消费。
// 错误细节不编码进状态字符串，统一放入安全摘要字段。
// 重复结束步骤由 Scope 拦截，不会在这里产生额外状态。
// 未知枚举只可能来自非法强制转换，防御性返回 unknown。
const char* traceStepStatusToString(TraceStepStatus status) noexcept {
    switch(status) {
        case TraceStepStatus::Running:
            return "running";
        case TraceStepStatus::Success:
            return "success";
        case TraceStepStatus::Error:
            return "error";
    }
    return "unknown";
}

// 详情分类只会传给调用方脱敏器或用于诊断展示。
// SDK 不根据字符串反向解析类别，MVP 因此不提供反序列化。
// 返回值与枚举一一对应，避免每个接入层自定义名称。
// 函数保持 noexcept，防止 Trace 辅助逻辑影响主流程。
// ModelRequest 与 ModelResponse 分离，支持不同脱敏强度。
// ToolArguments 与 ToolResult 分离，支持只记录输入或输出摘要。
// 新增类别必须同时说明其原始数据边界。
const char* traceDetailKindToString(TraceDetailKind kind) noexcept {
    switch(kind) {
        case TraceDetailKind::ModelRequest:
            return "model_request";
        case TraceDetailKind::ModelResponse:
            return "model_response";
        case TraceDetailKind::ToolArguments:
            return "tool_arguments";
        case TraceDetailKind::ToolResult:
            return "tool_result";
    }
    return "unknown";
}

// 属性名称是导出 JSON 的稳定白名单，而不是内部调试标签。
// 映射集中在此处，接入层只能持有枚举，无法构造任意属性键。
// Provider 和 Model 名称用于请求路由聚合，不包含请求正文。
// stream 与 callback_present 解释控制流分支，不保存回调对象。
// message_count 和工具相关 count 只保存数量，不保存集合内容。
// Token 字段沿用统一响应结构，缺失时由上层写入其默认数值。
// HTTP 方法当前只会写 POST，但仍保留独立键以支持后续扩展。
// timeout_ms 是配置事实，不代表实际等待时间；耗时使用 duration_ms。
// http_status_code 仅在收到响应后出现，纯网络失败不会伪造状态码。
// response_bytes 与 chunk_count 用于容量诊断，不包含分块内容。
// SSE 四类计数分别表达增量、工具增量、结束和错误事件。
// event_count 由分类计数求和，消费者可用于快速完整性校验。
// tool_name 属于确认过的默认元数据，但参数和结果仍必须脱敏。
// tool_index 使用模型返回顺序，不能被并行完成顺序覆盖。
// success_count 与 failure_count 的和应等于批次 tool_count。
// 单工具 success 表达业务结果，不等同于步骤记录过程是否成功。
// 字符串采用小写下划线形式，方便跨语言索引和长期存储。
// 未知枚举防御性返回 unknown，Recorder 会在写入前拒绝其值。
const char* traceAttributeKeyToString(TraceAttributeKey key) noexcept {
    switch(key) {
        case TraceAttributeKey::Provider:
            return "provider";
        case TraceAttributeKey::Model:
            return "model";
        case TraceAttributeKey::Stream:
            return "stream";
        case TraceAttributeKey::MessageCount:
            return "message_count";
        case TraceAttributeKey::ToolDefinitionCount:
            return "tool_definition_count";
        case TraceAttributeKey::ToolCallCount:
            return "tool_call_count";
        case TraceAttributeKey::PromptTokens:
            return "prompt_tokens";
        case TraceAttributeKey::CompletionTokens:
            return "completion_tokens";
        case TraceAttributeKey::TotalTokens:
            return "total_tokens";
        case TraceAttributeKey::CallbackPresent:
            return "callback_present";
        case TraceAttributeKey::HttpMethod:
            return "method";
        case TraceAttributeKey::TimeoutMs:
            return "timeout_ms";
        case TraceAttributeKey::HttpStatusCode:
            return "http_status_code";
        case TraceAttributeKey::ResponseBytes:
            return "response_bytes";
        case TraceAttributeKey::ChunkCount:
            return "chunk_count";
        case TraceAttributeKey::DeltaCount:
            return "delta_count";
        case TraceAttributeKey::ToolCallDeltaCount:
            return "tool_call_delta_count";
        case TraceAttributeKey::DoneCount:
            return "done_count";
        case TraceAttributeKey::ErrorCount:
            return "error_count";
        case TraceAttributeKey::EventCount:
            return "event_count";
        case TraceAttributeKey::ToolCount:
            return "tool_count";
        case TraceAttributeKey::SuccessCount:
            return "success_count";
        case TraceAttributeKey::FailureCount:
            return "failure_count";
        case TraceAttributeKey::ToolName:
            return "tool_name";
        case TraceAttributeKey::ToolIndex:
            return "tool_index";
        case TraceAttributeKey::Success:
            return "success";
    }
    return "unknown";
}

// 详情槽位与原始数据类别一一对应，不能由调用方自由命名。
// request 和 response 只属于模型公开操作的根步骤。
// arguments 和 result 只属于单工具执行步骤。
// 固定槽位让消费者无需扫描动态键即可判断详情是否存在。
// 同一槽位在单个步骤内最多保存一次最终处理结果。
// 槽位本身不表示详情安全，安全性由脱敏器返回对象决定。
// Recorder 会在回调前校验类别与槽位匹配，错配时直接丢弃。
// unknown 只处理非法枚举，不作为可导出的正常协议值。
const char* traceDetailSlotToString(TraceDetailSlot slot) noexcept {
    switch(slot) {
        case TraceDetailSlot::Request:
            return "request";
        case TraceDetailSlot::Response:
            return "response";
        case TraceDetailSlot::Arguments:
            return "arguments";
        case TraceDetailSlot::Result:
            return "result";
    }
    return "unknown";
}

// 详情状态把“有安全内容”和“没有可保存内容”明确分开。
// recorded 表示 value 是脱敏器显式返回的顶层对象。
// rejected 表示脱敏器正常返回，但结果不是允许的对象形状。
// sanitizer_failed 表示调用方脱敏逻辑抛出异常。
// 后两种状态的 value 始终为空对象，不保存原始输入或异常文本。
// 状态属于结构化诊断，消费者不需要解析中文错误摘要。
// 详情失败不会改变步骤业务状态，也不会结束当前步骤。
// unknown 是非法枚举的防御值，不会由 Recorder 主动生成。
const char* traceDetailStatusToString(TraceDetailStatus status) noexcept {
    switch(status) {
        case TraceDetailStatus::Recorded:
            return "recorded";
        case TraceDetailStatus::Rejected:
            return "rejected";
        case TraceDetailStatus::SanitizerFailed:
            return "sanitizer_failed";
    }
    return "unknown";
}

// 失败机器码用于稳定查询、聚合和自动告警，不用于展示原始错误。
// none 与成功步骤绑定，保证每个步骤始终具有可读取的错误码字段。
// 生命周期类错误只描述 Scope 使用事实，不猜测业务异常原因。
// model 与 provider 分类分别标识公开门面和供应商协议边界。
// 同步与流式模型请求分开，便于统计回调链路特有故障。
// HTTP 非 2xx 保留状态码事实，与无响应的传输失败严格区分。
// 流式传输失败表示网络读写失败，不包含用户回调抛出的异常。
// stream_callback_failed 专门表示下游回调终止了传输读取。
// SSE Error 事件来自协议内容，即使公开流式调用没有抛异常也可出现。
// SSE incomplete 表示成功响应中没有观察到 Done 事件。
// SSE processing failed 覆盖事件解析、缓冲冲刷或回调传播失败。
// 工具执行失败属于单个结果，可与同批其他成功结果并存。
// 工具批次异常表示容器或执行循环无法继续，原异常仍向上传播。
// 工具批次部分失败表示循环完成，但至少一个工具返回失败结果。
// 机器码不得包含工具名、URL、状态正文或 exception.what()。
// 新增失败类别时必须同步中文摘要、文档和序列化测试。
// 字符串采用小写下划线，避免依赖 C++ 枚举名称或编译器信息。
// unknown 只作为非法强转兜底，正常接入不得依赖它。
const char* traceFailureToCode(TraceFailure failure) noexcept {
    switch(failure) {
        case TraceFailure::None:
            return "none";
        case TraceFailure::StepAbandoned:
            return "step_abandoned";
        case TraceFailure::ScopeReplaced:
            return "scope_replaced";
        case TraceFailure::TraceRecordingFailed:
            return "trace_recording_failed";
        case TraceFailure::ModelRequestFailed:
            return "model_request_failed";
        case TraceFailure::StreamModelRequestFailed:
            return "stream_model_request_failed";
        case TraceFailure::ProviderRequestFailed:
            return "provider_request_failed";
        case TraceFailure::ProviderStreamFailed:
            return "provider_stream_failed";
        case TraceFailure::HttpStatusFailed:
            return "http_status_failed";
        case TraceFailure::HttpTransportFailed:
            return "http_transport_failed";
        case TraceFailure::HttpStreamFailed:
            return "http_stream_failed";
        case TraceFailure::StreamCallbackFailed:
            return "stream_callback_failed";
        case TraceFailure::SseErrorEvent:
            return "sse_error_event";
        case TraceFailure::SseIncomplete:
            return "sse_incomplete";
        case TraceFailure::SseProcessingFailed:
            return "sse_processing_failed";
        case TraceFailure::ToolExecutionFailed:
            return "tool_execution_failed";
        case TraceFailure::ToolBatchException:
            return "tool_batch_exception";
        case TraceFailure::ToolBatchPartialFailure:
            return "tool_batch_partial_failure";
    }
    return "unknown";
}

// 中文摘要面向人工排障，但仍属于默认安全 Trace 数据。
// 摘要只说明当前步骤观察到的固定事实，不复述底层原始异常。
// 同一失败枚举必须始终返回同一摘要，避免日志语言随调用点漂移。
// 成功步骤返回空摘要，JSON 仍保留字段以维持固定结构。
// 生命周期摘要帮助发现接入层漏掉 succeed/fail，而不暴露业务输入。
// 模型层摘要说明公开调用失败，不替代 Provider 或 HTTP 子步骤信息。
// Provider 摘要只说明协议处理失败，远端错误正文仍由原异常承载。
// HTTP 状态摘要与 status_code 属性配合定位，不复制响应正文。
// 网络摘要不包含 URL、代理、证书或底层库消息。
// 回调摘要不包含用户抛出的 what()，原异常按旧契约继续传播。
// SSE 摘要只表达错误事件、结束标记缺失或处理失败三类事实。
// 工具摘要不包含参数、结果或处理函数异常文本。
// 批次部分失败摘要不隐含重试策略，是否重试仍由上层决定。
// 所有可显示文本使用简体中文，机器消费应优先读取 error_code。
// 摘要不是本地化框架，修改既有文本仍需视为公开协议迁移。
// 未知失败的兜底文本同样不读取任何调用方数据。
// 映射保持 noexcept，异常展开期间结束步骤不会产生第二个异常。
// 新摘要必须先经过敏感信息审查，再允许加入默认导出。
const char* traceFailureToSummary(TraceFailure failure) noexcept {
    switch(failure) {
        case TraceFailure::None:
            return "";
        case TraceFailure::StepAbandoned:
            return "步骤未正常结束";
        case TraceFailure::ScopeReplaced:
            return "步骤作用域被替换";
        case TraceFailure::TraceRecordingFailed:
            return "Trace 步骤记录失败";
        case TraceFailure::ModelRequestFailed:
            return "模型请求失败";
        case TraceFailure::StreamModelRequestFailed:
            return "流式模型请求失败";
        case TraceFailure::ProviderRequestFailed:
            return "Provider 请求处理失败";
        case TraceFailure::ProviderStreamFailed:
            return "Provider 流式请求处理失败";
        case TraceFailure::HttpStatusFailed:
            return "HTTP 返回非成功状态";
        case TraceFailure::HttpTransportFailed:
            return "HTTP 传输失败";
        case TraceFailure::HttpStreamFailed:
            return "HTTP 流式传输失败";
        case TraceFailure::StreamCallbackFailed:
            return "流式回调处理失败";
        case TraceFailure::SseErrorEvent:
            return "SSE 返回错误事件";
        case TraceFailure::SseIncomplete:
            return "SSE 流未正常结束";
        case TraceFailure::SseProcessingFailed:
            return "SSE 流处理失败";
        case TraceFailure::ToolExecutionFailed:
            return "工具执行失败";
        case TraceFailure::ToolBatchException:
            return "工具批次执行异常";
        case TraceFailure::ToolBatchPartialFailure:
            return "工具批次包含失败结果";
    }
    return "Trace 失败原因未知";
}

// 单步骤序列化固定输出所有顶层字段。
// 调用方无需区分“字段不存在”和“当前没有父节点或错误”。
// attributes 与 details 已经是快照副本，此处不会触碰会话锁。
// 序列化失败只会由显式 toJson 调用方感知，不在业务埋点中执行。
// 根步骤使用 null 而不是空字符串，避免混淆无父节点与坏 ID。
// error_summary 即使为空也保留，消费者不需要做字段存在性判断。
// JSON 对象键顺序不作为契约，字段名称和类型才是稳定边界。
nlohmann::json traceStepToJson(const TraceStep& step) {
    // parent_step_id 始终导出；根步骤使用 null，避免调用方猜测字段是否缺失。
    const nlohmann::json parent_step_id =
        step.parent_step_id.has_value() ? nlohmann::json(*step.parent_step_id) : nlohmann::json(nullptr);

    return nlohmann::json{
        {"step_id",        step.step_id                        },
        {"parent_step_id", parent_step_id                      },
        {"sequence",       step.sequence                       },
        {"type",           traceStepTypeToString(step.type)    },
        {"status",         traceStepStatusToString(step.status)},
        {"started_at",     step.started_at                     },
        {"duration_ms",    step.duration_ms                    },
        {"attributes",     step.attributes                     },
        {"details",        step.details                        },
        {"error_code",     traceFailureToCode(step.failure)    },
        {"error_summary",  step.error_summary                  },
    };
}

// Trace 导出保持步骤输入顺序；TraceSession::snapshot 已按 sequence 排序。
// 这里不再次排序，避免隐藏调用方手工构造 Trace 时的顺序问题。
// steps 逐个转换可维持统一字段契约，并避免依赖隐式 ADL 序列化。
// 返回 JSON 值由调用方自由保存、打印或传给其他系统。
// Trace 本身不包含 Provider 或 Model，因为同一会话可跨多次请求。
// 会话总耗时也不在顶层计算，因为显式会话没有固定结束时刻。
// 禁用会话仍导出空 trace_id 和空数组，便于统一消费。
nlohmann::json traceToJson(const Trace& trace) {
    nlohmann::json steps = nlohmann::json::array();
    for(const auto& step : trace.steps) {
        steps.push_back(traceStepToJson(step));
    }

    return nlohmann::json{
        {"trace_id", trace.trace_id  },
        {"steps",    std::move(steps)},
    };
}

}  // namespace aiSDK
