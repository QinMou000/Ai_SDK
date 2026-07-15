#include "AIClient.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "provider/DeepSeekProvider.h"

namespace aiSDK {
namespace {

// findProviderConfig 负责从全局配置中提取指定 Provider 的配置块。
ProviderConfig findProviderConfig(const Config& config, const std::string& provider_name) {
    const auto iterator = config.providers.find(provider_name);
    if(iterator == config.providers.end()) {
        return ProviderConfig{};
    }

    return iterator->second;
}

// createProvider 根据名称构造具体 Provider。
// 当前阶段只正式支持 DeepSeek，其他名称会明确报错。
std::shared_ptr<IModelProvider> createProvider(const Config& config, const std::string& provider_name) {
    if(provider_name == "deepseek") {
        return std::make_shared<DeepSeekProvider>(findProviderConfig(config, provider_name), config.timeout_ms);
    }

    throw std::invalid_argument("不支持的 Provider: " + provider_name);
}

}  // namespace

AIClient::AIClient(Config config) : config_(std::move(config)), active_provider_name_(config_.default_provider) {
    active_provider_ = createProvider(config_, active_provider_name_);
}

const Config& AIClient::config() const {
    return config_;
}

const std::string& AIClient::activeProviderName() const {
    return active_provider_name_;
}

TraceSession AIClient::startTrace(TraceOptions options) const {
    // 会话由调用方显式持有，可跨一次模型请求和后续工具执行累积完整链路。
    // AIClient 只决定是否允许采集，不隐式缓存会话，避免不同业务请求串链。
    // 关闭时立即返回默认空句柄，避免生成随机 ID、读取时钟或分配共享状态。
    if(!config_.enable_trace) {
        return TraceSession{};
    }
    // 启用会话时保留调用方传入的详情策略和脱敏器，不额外放宽采集范围。
    return TraceSession::createEnabled(std::move(options));
}

ChatResponse AIClient::chat(const ChatRequest& request) const {
    if(!active_provider_) {
        throw std::logic_error("当前 Provider 尚未初始化");
    }

    return active_provider_->chat(request);
}

ChatResponse AIClient::chat(const ChatRequest& request, TraceSession& trace_session) const {
    // 当前客户端的总开关优先于外部会话，避免复用其他客户端会话绕过禁用配置。
    // 禁用分支完全复用原业务入口，避免两套请求实现随迭代产生行为差异。
    if(!config_.enable_trace) {
        return chat(request);
    }
    if(!active_provider_) {
        throw std::logic_error("当前 Provider 尚未初始化");
    }

    // Recorder 只借用显式会话；请求完成后，快照和导出仍由调用方控制。
    // ModelRequest 是本次调用的根步骤，Provider、HTTP 等步骤均以它为父节点。
    TraceRecorder recorder(trace_session);
    // 会话自身也可能处于禁用状态，此时 scope 会退化为可安全调用的空对象。
    TraceStepScope request_step = recorder.startStep(TraceStepType::ModelRequest);
    if(request_step.enabled()) {
        // 根步骤只写入白名单元数据，默认不采集消息正文和工具参数。
        // 请求未指定模型时不猜测 Provider 的默认值，实际解析值由下层步骤记录。
        request_step.setAttribute(TraceAttributeKey::Provider, active_provider_name_);
        if(!request.model.empty()) {
            request_step.setAttribute(TraceAttributeKey::Model, request.model);
        }
        // 数量信息足以支持容量和用量分析，同时避免复制可能含敏感内容的对象。
        request_step.setAttribute(TraceAttributeKey::Stream, false);
        request_step.setAttribute(TraceAttributeKey::MessageCount, request.messages.size());
        request_step.setAttribute(TraceAttributeKey::ToolDefinitionCount, request.tools.size());
    }
    if(request_step.wantsDetails()) {
        // 只有显式开启详情且提供脱敏器时才构造详情输入，默认路径保持零内容采集。
        // 原始请求只会作为脱敏器输入；TraceStepScope 不会在内部保留该值。
        try {
            // 上下文携带 Provider 名称，便于同一脱敏器按供应商规则裁剪字段。
            const TraceDetailContext context{TraceDetailKind::ModelRequest, active_provider_name_};
            request_step.setSanitizedDetail(TraceDetailSlot::Request, context, chatRequestToJson(request));
        } catch(...) {
            // 详情准备属于旁路能力，序列化失败不能阻止真实模型请求。
            // 此处吞掉的仅是 Trace 详情异常，后续 Provider 的业务异常仍正常传播。
        }
    }

    try {
        // 仅在根步骤有效时传递父 ID，确保成功链路具有单一、稳定的根节点。
        // 根步骤创建失败时不继续创建无父节点的下层步骤，业务请求仍照常执行。
        ChatResponse response = request_step.enabled()
                                    ? active_provider_->chat(request, trace_session, request_step.stepId())
                                    : active_provider_->chat(request);
        if(request_step.enabled()) {
            // 响应统计在 Provider 返回后统一写入，失败请求不会留下伪造的用量数据。
            // 工具调用仅记录数量；名称、参数和结果由后续工具执行步骤按策略处理。
            // 用量字段保留 Provider 的原始计数口径，门面层不做估算或补偿。
            request_step.setAttribute(TraceAttributeKey::ToolCallCount, response.tool_calls.size());
            request_step.setAttribute(TraceAttributeKey::PromptTokens, response.usage.prompt_tokens);
            request_step.setAttribute(TraceAttributeKey::CompletionTokens, response.usage.completion_tokens);
            request_step.setAttribute(TraceAttributeKey::TotalTokens, response.usage.total_tokens);
        }
        if(request_step.wantsDetails()) {
            // 响应详情与请求详情采用独立槽位，便于导出方分别授权和审计。
            // 完整响应可能含 raw_response，只有调用方显式脱敏后的对象才会写入 Trace。
            try {
                // 同一 Provider 上下文保证请求和响应可以使用一致的字段策略。
                const TraceDetailContext context{TraceDetailKind::ModelResponse, active_provider_name_};
                request_step.setSanitizedDetail(TraceDetailSlot::Response, context, chatResponseToJson(response));
            } catch(...) {
                // 响应详情准备失败时仍原样返回 ChatResponse。
                // 已取得的模型响应不会因观测旁路失败而被替换或丢弃。
            }
        }
        // 业务响应和可选详情均处理完毕后才结束步骤，耗时覆盖完整门面调用。
        request_step.succeed();
        return response;
    } catch(...) {
        // 固定摘要不复制底层 URL、服务端正文或异常 what()，原异常继续原样传播。
        // fail 本身是旁路收尾；随后重新抛出可保留原始异常类型和调用栈语义。
        request_step.fail(TraceFailure::ModelRequestFailed);
        throw;
    }
}

void AIClient::streamChat(const ChatRequest& request, StreamCallback callback) const {
    if(!active_provider_) {
        throw std::logic_error("当前 Provider 尚未初始化");
    }

    active_provider_->streamChat(request, std::move(callback));
}

void AIClient::streamChat(const ChatRequest& request, StreamCallback callback, TraceSession& trace_session) const {
    // 关闭总开关时直接走原入口，不创建任何 Trace 步骤。
    // 该路径也保留空回调校验位置，Trace 开关不会改变既有前置条件。
    if(!config_.enable_trace) {
        streamChat(request, std::move(callback));
        return;
    }
    if(!active_provider_) {
        throw std::logic_error("当前 Provider 尚未初始化");
    }

    // 流式请求沿用非流式请求的根类型，通过 stream 属性区分协议形态。
    // 根步骤覆盖从发起请求到流式回调全部结束的端到端耗时。
    TraceRecorder recorder(trace_session);
    TraceStepScope request_step = recorder.startStep(TraceStepType::ModelRequest);
    if(request_step.enabled()) {
        // 回调是否存在只记录布尔事实，不尝试检查或调用用户对象。
        request_step.setAttribute(TraceAttributeKey::Provider, active_provider_name_);
        if(!request.model.empty()) {
            request_step.setAttribute(TraceAttributeKey::Model, request.model);
        }
        // 输入统计与同步入口保持相同口径，便于调用方直接比较两种模式。
        request_step.setAttribute(TraceAttributeKey::Stream, true);
        request_step.setAttribute(TraceAttributeKey::MessageCount, request.messages.size());
        request_step.setAttribute(TraceAttributeKey::ToolDefinitionCount, request.tools.size());
        request_step.setAttribute(TraceAttributeKey::CallbackPresent, static_cast<bool>(callback));
    }
    if(request_step.wantsDetails()) {
        // 流式阶段只采集一次请求详情，事件增量由 SSE 子步骤汇总为计数。
        // 不在门面缓存流式响应片段，避免为观测功能引入额外内存增长。
        try {
            const TraceDetailContext context{TraceDetailKind::ModelRequest, active_provider_name_};
            request_step.setSanitizedDetail(TraceDetailSlot::Request, context, chatRequestToJson(request));
        } catch(...) {
            // 详情准备失败不改变空回调或流式请求的既有控制流。
        }
    }

    try {
        // 回调所有权只向实际执行分支移动一次，降级分支不会访问已移动对象。
        // 根步骤不可用时退回原接口，避免下层 Trace 误形成孤立根节点。
        if(request_step.enabled()) {
            active_provider_->streamChat(request, std::move(callback), trace_session, request_step.stepId());
        } else {
            active_provider_->streamChat(request, std::move(callback));
        }
        // Provider 正常返回表示流已按其协议结束，根步骤才可标记成功。
        request_step.succeed();
    } catch(...) {
        // 回调异常和 Provider 异常都保持既有传播方式，Trace 只记录安全失败事实。
        // 更细的 HTTP、SSE 或回调分类由下层子步骤承担，门面仅表达整体失败。
        request_step.fail(TraceFailure::StreamModelRequestFailed);
        throw;
    }
}

ToolRegistry& AIClient::tools() {
    return tool_registry_;
}

const ToolRegistry& AIClient::tools() const {
    return tool_registry_;
}

std::vector<ToolExecutionResult> AIClient::executeToolCalls(const std::vector<ToolCall>& calls) {
    // AIClient 只把门面持有的注册表交给通用执行器，
    // 不在这里识别 DeepSeek 字段，也不复制 Provider 的协议解析逻辑。
    // 执行器只在本次调用期间借用注册表，避免引入额外生命周期状态；
    // 空 calls 会自然返回空结果，不触发网络请求或任何隐藏副作用。
    ToolExecutor executor(tool_registry_);
    return executor.executeAll(calls);
}

std::vector<ToolExecutionResult> AIClient::executeToolCalls(const std::vector<ToolCall>& calls,
                                                            TraceSession& trace_session) {
    // 总开关关闭时复用原入口，确保外部启用会话也不能产生工具 Trace。
    // 开关仅控制观测能力，工具注册表和结果顺序不受会话状态影响。
    if(!config_.enable_trace) {
        return executeToolCalls(calls);
    }
    // 批次和单工具步骤均由 ToolExecutor 统一记录，避免门面产生重复语义步骤。
    // 该重载仍不追加消息或自动发起后续模型请求。
    // 调用方可复用模型请求会话，从而通过序号还原请求后的工具执行阶段。
    ToolExecutor executor(tool_registry_);
    return executor.executeAll(calls, trace_session);
}

void AIClient::setProvider(const std::string& provider_name) {
    if(provider_name.empty()) {
        throw std::invalid_argument("Provider 名称不能为空");
    }

    active_provider_ = createProvider(config_, provider_name);
    active_provider_name_ = provider_name;
}

void AIClient::setProvider(std::shared_ptr<IModelProvider> provider) {
    // 注入入口用于测试和扩展 Provider，但仍要求实例具备可审计的稳定名称。
    if(!provider) {
        throw std::invalid_argument("Provider 实例不能为空");
    }

    // info 仅在提交替换前读取一次，避免状态更新后再次调用产生不一致结果。
    const ProviderInfo provider_info = provider->info();
    if(provider_info.name.empty()) {
        throw std::invalid_argument("Provider 名称不能为空");
    }

    // 所有校验完成后再替换当前状态，避免失败注入污染原 Provider。
    active_provider_ = std::move(provider);
    active_provider_name_ = provider_info.name;
}

}  // namespace aiSDK
