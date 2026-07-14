#include "provider/DeepSeekProvider.h"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "core/ChatResponse.h"
#include "core/Message.h"
#include "tool/Tool.h"

namespace aiSDK {
namespace {

// jsonStringOrEmpty 在字段缺失、为 null 或类型不匹配时统一返回空字符串。
// 这样 Provider 解析层可以兼容 tool_call 和普通回答中的可空文本字段。
std::string jsonStringOrEmpty(const nlohmann::json& json, const char* key) {
    if(!json.contains(key) || json.at(key).is_null()) {
        return "";
    }

    if(!json.at(key).is_string()) {
        return "";
    }

    return json.at(key).get<std::string>();
}

// kDeepSeekProviderName 统一维护运行时 Provider 标识。
constexpr const char* kDeepSeekProviderName = "deepseek";
// kDeepSeekDefaultBaseUrl 对齐 DeepSeek 官方 OpenAI-compatible 基础地址。
constexpr const char* kDeepSeekDefaultBaseUrl = "https://api.deepseek.com";
// kDeepSeekDefaultModel 默认使用当前官方长期模型名，避免继续依赖即将废弃的别名。
constexpr const char* kDeepSeekDefaultModel = "deepseek-v4-flash";

// trimTrailingSlash 负责去掉末尾斜杠，避免拼接路径时出现双斜杠。
std::string trimTrailingSlash(std::string text) {
    while(!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

// resolveBaseUrl 在未显式配置时回退到官方默认地址。
std::string resolveBaseUrl(const ProviderConfig& config) {
    if(config.base_url.empty()) {
        return kDeepSeekDefaultBaseUrl;
    }
    return trimTrailingSlash(config.base_url);
}

// resolveModelName 在请求模型、配置默认模型和库默认模型之间做兜底选择。
std::string resolveModelName(const ProviderConfig& config, const ChatRequest& request) {
    if(!request.model.empty()) {
        return request.model;
    }
    if(!config.default_model.empty()) {
        return config.default_model;
    }
    return kDeepSeekDefaultModel;
}

// requireApiKey 保证真实调用前已经注入密钥。
std::string requireApiKey(const ProviderConfig& config) {
    if(config.api_key.empty()) {
        throw std::invalid_argument("DeepSeekProvider 缺少 api_key，请通过配置或环境变量提供");
    }
    return config.api_key;
}

// buildChatUrl 统一构造 Chat Completions 终端地址。
std::string buildChatUrl(const ProviderConfig& config) {
    return resolveBaseUrl(config) + "/chat/completions";
}

// toolCallToProviderJson 把内部 ToolCall 映射为 OpenAI-compatible assistant tool_calls 结构。
nlohmann::json toolCallToProviderJson(const ToolCall& call) {
    const std::string arguments = call.raw_arguments.empty() ? call.arguments.dump() : call.raw_arguments;

    return nlohmann::json{
        {"id",       call.id   },
        {"type",     "function"},
        {"function",
         {
             {"name", call.name},
             {"arguments", arguments},
         }                     },
    };
}

// messageToProviderJson 把统一消息结构转换为 DeepSeek 请求消息。
nlohmann::json messageToProviderJson(const Message& message) {
    nlohmann::json json{
        {"role",    roleToString(message.role)},
        {"content", message.content           },
    };

    if(message.name.has_value()) {
        json["name"] = *message.name;
    }
    if(message.tool_call_id.has_value()) {
        json["tool_call_id"] = *message.tool_call_id;
    }
    if(!message.tool_calls.empty()) {
        json["tool_calls"] = nlohmann::json::array();
        for(const auto& call : message.tool_calls) {
            json["tool_calls"].push_back(toolCallToProviderJson(call));
        }
    }

    return json;
}

// toolToProviderJson 把统一工具定义转换为 OpenAI-compatible tools 描述。
nlohmann::json toolToProviderJson(const Tool& tool) {
    return nlohmann::json{
        {"type",     "function"},
        {"function",
         {
             {"name", tool.name},
             {"description", tool.description},
             {"parameters", tool.parameters},
         }                     },
    };
}

// buildRequestJson 统一生成 DeepSeek 请求体，避免 chat / stream 两处手工维护字段。
nlohmann::json buildRequestJson(const ProviderConfig& config, const ChatRequest& request, bool stream) {
    nlohmann::json json{
        {"model", resolveModelName(config, request)},
        {"messages", nlohmann::json::array()},
        {"stream", stream},
        {"temperature", request.temperature},
        {"max_tokens", request.max_tokens},
    };

    for(const auto& message : request.messages) {
        json["messages"].push_back(messageToProviderJson(message));
    }

    if(!request.tools.empty()) {
        json["tools"] = nlohmann::json::array();
        for(const auto& tool : request.tools) {
            json["tools"].push_back(toolToProviderJson(tool));
        }
    }

    if(stream) {
        // include_usage 让最终流式阶段返回完整 token 统计，便于后续补 usage 能力。
        json["stream_options"] = nlohmann::json{
            {"include_usage", true}
        };
    }

    return json;
}

// buildHeaders 统一注入 Content-Type 与 Bearer 认证头。
HttpHeaders buildHeaders(const ProviderConfig& config) {
    return HttpHeaders{
        {"Content-Type",  "application/json"               },
        {"Authorization", "Bearer " + requireApiKey(config)},
    };
}

// parseToolCall 把 DeepSeek/OpenAI-compatible tool call 结构转换回内部对象。
ToolCall parseToolCall(const nlohmann::json& json) {
    ToolCall call;
    call.id = jsonStringOrEmpty(json, "id");

    if(json.contains("function")) {
        const nlohmann::json& function = json.at("function");
        call.name = jsonStringOrEmpty(function, "name");
        call.raw_arguments = jsonStringOrEmpty(function, "arguments");

        if(!call.raw_arguments.empty()) {
            const nlohmann::json arguments = nlohmann::json::parse(call.raw_arguments, nullptr, false);
            call.arguments = arguments.is_discarded() ? nlohmann::json::object() : arguments;
        }
    }

    return call;
}

// parseUsage 提取统一 token 统计结构；缺失时返回默认值。
Usage parseUsage(const nlohmann::json& json) {
    Usage usage;
    if(!json.is_object()) {
        return usage;
    }

    usage.prompt_tokens = json.value("prompt_tokens", 0);
    usage.completion_tokens = json.value("completion_tokens", 0);
    usage.total_tokens = json.value("total_tokens", 0);
    return usage;
}

// extractHttpError 尝试优先提取服务端结构化错误信息。
std::string extractHttpError(const HttpResponse& response) {
    const nlohmann::json json = nlohmann::json::parse(response.body, nullptr, false);
    if(!json.is_discarded() && json.contains("error")) {
        const nlohmann::json& error = json.at("error");
        if(error.is_object() && error.contains("message")) {
            return error.at("message").get<std::string>();
        }
        return error.dump();
    }

    if(!response.body.empty()) {
        return response.body;
    }

    return "空响应体";
}

// ensureSuccessStatus 把非 2xx 响应转换成明确异常，便于调用方快速定位问题。
void ensureSuccessStatus(const HttpResponse& response, const std::string& url) {
    if(response.status_code >= 200 && response.status_code < 300) {
        return;
    }

    std::ostringstream message;
    message << "DeepSeek API 请求失败: " << url << " | HTTP " << response.status_code << " | " << extractHttpError(response);
    throw std::runtime_error(message.str());
}

// parseChatResponse 解析普通 Chat Completion 响应体。
ChatResponse parseChatResponse(const std::string& raw_response) {
    const nlohmann::json json = nlohmann::json::parse(raw_response);
    const nlohmann::json& choice = json.at("choices").at(0);
    const nlohmann::json& message_json = choice.at("message");

    ChatResponse response;
    response.raw_response = raw_response;
    response.content = jsonStringOrEmpty(message_json, "content");
    response.message = AssistantMessage(response.content);
    response.usage = parseUsage(json.value("usage", nlohmann::json::object()));

    if(message_json.contains("tool_calls")) {
        for(const auto& tool_call_json : message_json.at("tool_calls")) {
            response.tool_calls.push_back(parseToolCall(tool_call_json));
        }
        response.message.tool_calls = response.tool_calls;
    }

    return response;
}

// processBufferedEvents 从累积缓冲区中提取完整 SSE 事件并回调给上层。
void processBufferedEvents(std::string& buffer, const SSEParser& parser, const StreamCallback& callback) {
    while(true) {
        // SSE 协议中，事件块之间的分隔符是两个连续换行符（\n\n 或 \r\n\r\n）。
        size_t boundary = buffer.find("\r\n\r\n");
        size_t delimiter_size = 4;

        const size_t lf_boundary = buffer.find("\n\n");
        // 如果同时存在 \r\n\r\n 和 \n\n，优先使用最先出现的那个作为边界。
        if(boundary == std::string::npos || (lf_boundary != std::string::npos && lf_boundary < boundary)) {
            boundary = lf_boundary;
            delimiter_size = 2;
        }

        if(boundary == std::string::npos) {
            return;
        }

        const std::string event_block = buffer.substr(0, boundary);
        buffer.erase(0, boundary + delimiter_size);

        // 解析器只处理完整事件块，避免半包数据导致 JSON 解析失败。
        for(const auto& event : parser.parseChunk(event_block)) {
            callback(event);
        }
    }
}

}  // namespace

// 默认构造路径委托给可注入构造路径，避免生产与测试初始化规则分叉。
// HttpClient 作为值成员固定在 Provider 生命周期内，单次请求不会切换传输实现。
DeepSeekProvider::DeepSeekProvider(ProviderConfig config, int timeout_ms) : DeepSeekProvider(std::move(config), timeout_ms, HttpClient{}) {}

// 传输注入只影响网络执行方式，不改变请求映射、响应解析或 Trace 层级。
// 这为失败、分块和边界条件测试提供确定输入，同时保留真实 Provider 行为。
DeepSeekProvider::DeepSeekProvider(ProviderConfig config, int timeout_ms, HttpClient http_client)
    : config_(std::move(config)), timeout_ms_(timeout_ms), http_client_(std::move(http_client)) {}

ChatResponse DeepSeekProvider::chat(const ChatRequest& request) {
    // 无 Trace 入口复用同一实现，禁用句柄不会分配步骤或详情。
    TraceSession disabled_trace;
    return chat(request, disabled_trace, "");
}

ChatResponse DeepSeekProvider::chat(const ChatRequest& request, TraceSession& trace_session, const std::string& parent_step_id) {
    // Provider 步骤承接 AIClient 父节点，并作为 HTTP 请求的直接父节点。
    // 步骤不可用时仍执行原业务路径，观测能力不能成为请求前置条件。
    TraceRecorder recorder(trace_session);
    TraceStepScope provider_step = recorder.startStep(TraceStepType::ProviderRequest, parent_step_id);
    if(provider_step.enabled()) {
        // Provider、最终模型和流式标志均来自受控白名单，不记录请求内容。
        // 模型在本层解析默认值，确保 Trace 与实际发往服务端的名称一致。
        provider_step.setAttribute(TraceAttributeKey::Provider, kDeepSeekProviderName);
        // 兜底函数返回字符串副本，其分配也必须纳入旁路异常隔离。
        try {
            provider_step.setAttribute(TraceAttributeKey::Model, resolveModelName(config_, request));
        } catch(...) {
            // 模型名副本只服务 Trace；分配失败时丢弃属性，真实请求仍继续。
        }
        provider_step.setAttribute(TraceAttributeKey::Stream, false);
    }

    try {
        // URL、请求体和认证头只在业务局部变量中流转，永不写入 Trace。
        // 先完成请求准备，可将密钥缺失准确归因到 Provider 而非 HTTP。
        const std::string url = buildChatUrl(config_);
        const nlohmann::json request_json = buildRequestJson(config_, request, false);
        const HttpHeaders headers = buildHeaders(config_);
        // Provider 步骤不可用时回到原 HTTP 入口，避免下层步骤成为孤立根节点。
        // 该降级路径同时防止 Trace 内部失败改变业务调用结果。
        const HttpResponse response = provider_step.enabled()
                                          ? http_client_.postJson(url, request_json, headers, timeout_ms_, trace_session, provider_step.stepId())
                                          : http_client_.postJson(url, request_json, headers, timeout_ms_);
        provider_step.setAttribute(TraceAttributeKey::HttpStatusCode, response.status_code);
        // HTTP 层记录传输事实，Provider 层负责解释 DeepSeek 的非成功响应。
        ensureSuccessStatus(response, url);

        // 仅在完整解析后记录安全汇总，半成品响应不会产生误导性成功指标。
        ChatResponse chat_response = parseChatResponse(response.body);
        if(provider_step.enabled()) {
            // 工具数量和 token 总量是白名单聚合值，不包含回答或工具参数。
            provider_step.setAttribute(TraceAttributeKey::ToolCallCount, chat_response.tool_calls.size());
            provider_step.setAttribute(TraceAttributeKey::TotalTokens, chat_response.usage.total_tokens);
        }
        provider_step.succeed();
        return chat_response;
    } catch(...) {
        // Provider 异常可能含远端正文或 URL，Trace 只保存固定安全摘要。
        // 固定失败枚举保持导出稳定，原异常仍完整交给业务调用方。
        provider_step.fail(TraceFailure::ProviderRequestFailed);
        throw;
    }
}

void DeepSeekProvider::streamChat(const ChatRequest& request, StreamCallback callback) {
    // 无 Trace 入口保持原有空回调和异常语义，只复用统一实现。
    TraceSession disabled_trace;
    streamChat(request, std::move(callback), disabled_trace, "");
}

void DeepSeekProvider::streamChat(const ChatRequest& request, StreamCallback callback, TraceSession& trace_session, const std::string& parent_step_id) {
    // 流式 Provider 步骤覆盖请求准备、传输和最终缓冲冲刷的完整生命周期。
    // SSE 步骤仅描述协议消费，HTTP 步骤仍独立描述网络执行。
    TraceRecorder recorder(trace_session);
    TraceStepScope provider_step = recorder.startStep(TraceStepType::ProviderRequest, parent_step_id);
    if(provider_step.enabled()) {
        // 白名单只保留路由与控制流元数据，不记录提示词或返回文本。
        // 回调存在性用于解释空操作，但不会序列化可调用对象本身。
        provider_step.setAttribute(TraceAttributeKey::Provider, kDeepSeekProviderName);
        try {
            provider_step.setAttribute(TraceAttributeKey::Model, resolveModelName(config_, request));
        } catch(...) {
            // Trace 属性准备失败不能把原本可执行的流式请求提前终止。
        }
        provider_step.setAttribute(TraceAttributeKey::Stream, true);
        provider_step.setAttribute(TraceAttributeKey::CallbackPresent, static_cast<bool>(callback));
    }

    // 空回调沿用原接口“不发起网络请求”的契约，并把这次空操作记录为成功。
    // 提前返回也会显式结束 Provider 步骤，避免 RAII 将其误判为中途放弃。
    if(!callback) {
        provider_step.succeed();
        return;
    }

    TraceStepScope sse_step;
    bool sse_step_attempted = false;
    // 四类事件分别计数，Trace 不保留任何 SSE 原始载荷。
    // 计数变量与同步流式调用同寿命，不会跨请求或跨线程共享。
    std::size_t delta_count = 0;
    std::size_t tool_call_delta_count = 0;
    std::size_t done_count = 0;
    std::size_t error_count = 0;
    bool http_status_failed = false;

    // SSE 步骤在首个响应分块到达时才创建，准备请求或纯传输失败不会被误归因到解析层。
    // 单次尝试标志避免创建失败后反复追加多个同类步骤。
    // Provider 步骤不可用时不创建孤立 SSE 根节点。
    auto ensure_sse_step = [&] {
        if(sse_step_attempted) {
            return;
        }
        sse_step_attempted = true;
        if(provider_step.enabled()) {
            sse_step = recorder.startStep(TraceStepType::SseStream, provider_step.stepId());
        }
    };

    // 汇总函数只写固定计数，不复制 delta、错误文本或原始 SSE 数据。
    // 计数发生在业务回调之前，因此抛异常的事件也会被纳入失败现场。
    // 回调异常继续向上传播，Trace 只观察而不恢复业务流程。
    auto traced_callback = [&](const StreamEvent& event) {
        switch(event.type) {
            case StreamEventType::Delta:
                ++delta_count;
                break;
            case StreamEventType::ToolCallDelta:
                ++tool_call_delta_count;
                break;
            case StreamEventType::Done:
                ++done_count;
                break;
            case StreamEventType::Error:
                ++error_count;
                break;
        }
        // 事件内容只交付给调用方，不经过 Trace 属性或详情容器。
        callback(event);
    };

    // SSE 与 HTTP 是 Provider 下的兄弟步骤，因为网络返回后仍可能冲刷残余缓冲。
    // 这避免 SSE 子步骤在其 HTTP 父步骤已经结束后继续运行。
    // 汇总写入集中在一个出口，成功与异常路径采用相同字段集合。
    // 空 Scope 的 enabled 检查使纯传输失败不会伪造 SSE 结果。
    auto write_sse_summary = [&] {
        if(!sse_step.enabled()) {
            return;
        }
        sse_step.setAttribute(TraceAttributeKey::DeltaCount, delta_count);
        sse_step.setAttribute(TraceAttributeKey::ToolCallDeltaCount, tool_call_delta_count);
        sse_step.setAttribute(TraceAttributeKey::DoneCount, done_count);
        sse_step.setAttribute(TraceAttributeKey::ErrorCount, error_count);
        // 总事件数由分类计数推导，避免独立累加造成口径漂移。
        sse_step.setAttribute(TraceAttributeKey::EventCount, delta_count + tool_call_delta_count + done_count + error_count);
    };

    try {
        // 请求准备位于 SSE 步骤创建之前，认证和序列化错误归属于 Provider。
        // 认证头仅传给 HttpClient，Trace 白名单没有承载密钥的入口。
        const std::string url = buildChatUrl(config_);
        const nlohmann::json request_json = buildRequestJson(config_, request, true);
        const HttpHeaders headers = buildHeaders(config_);
        std::string pending_buffer;  // 累积网络分块，直到遇到完整 SSE 事件块才交给解析器处理。

        // 回调同时服务有无 Trace 两条路径；禁用时 ensure_sse_step 只做一次廉价判断。
        // 网络分块与 SSE 事件边界并不等价，缓冲区负责跨分块拼接完整事件。
        // 缓冲区只存在于当前请求栈帧，结束后不会保留模型输出。
        HttpStreamCallback stream_callback = [&](std::string_view chunk) {
            ensure_sse_step();
            pending_buffer.append(chunk);
            processBufferedEvents(pending_buffer, sse_parser_, traced_callback);
        };
        const HttpResponse response =
            provider_step.enabled()
                // Trace 可用时显式传递父节点，维持 Provider 与 HTTP 的层级关系。
                ? http_client_.postJsonStream(url, request_json, headers, timeout_ms_, stream_callback, trace_session, provider_step.stepId())
                : http_client_.postJsonStream(url, request_json, headers, timeout_ms_, stream_callback);

        provider_step.setAttribute(TraceAttributeKey::HttpStatusCode, response.status_code);
        // 非成功状态先由 Provider 抛出，残余正文不会被误当成 SSE 尾事件冲刷。
        http_status_failed = response.status_code < 200 || response.status_code >= 300;
        ensureSuccessStatus(response, url);

        // 网络结束后再冲刷一次残留缓冲，避免最后一个事件块没有尾部分隔符。
        // 末尾冲刷仍使用相同追踪回调，保证事件统计口径一致。
        if(!pending_buffer.empty()) {
            for(const auto& event : sse_parser_.parseChunk(pending_buffer)) {
                traced_callback(event);
            }
        }

        // 成功响应即使没有正文也生成 SSE 步骤，以明确记录缺失 Done 的协议事实。
        // 该步骤表达协议完整性，不会为缺失 Done 新增业务异常。
        ensure_sse_step();
        write_sse_summary();
        if(error_count > 0U) {
            // SSE Error 事件原本不会抛异常，Trace 仅标记子步骤错误并保持回调契约。
            // Error 优先于缺失 Done，确保失败原因具有确定的分类顺序。
            sse_step.fail(TraceFailure::SseErrorEvent);
        } else if(done_count == 0U) {
            // 未观察到 Done 只形成可观测事实，不新增业务异常。
            // Provider 本身仍可成功结束，以保持既有流式接口的兼容语义。
            sse_step.fail(TraceFailure::SseIncomplete);
        } else {
            sse_step.succeed();
        }
        provider_step.succeed();
    } catch(...) {
        // 异常出口先固化已处理事件计数，再关闭相关步骤。
        write_sse_summary();
        if(http_status_failed) {
            // 非 2xx 由 HTTP 层负责，已经正常派发的 SSE 不再误报解析失败。
            // 若响应体明确产生 Error 事件，仍保留该协议事实。
            if(error_count > 0U) {
                sse_step.fail(TraceFailure::SseErrorEvent);
            } else {
                sse_step.succeed();
            }
        } else {
            // 回调、解析或部分流异常才归入 SSE 处理层；空 Scope 仍是安全无操作。
            sse_step.fail(TraceFailure::SseProcessingFailed);
        }
        // 原异常继续抛出，Trace 失败码不替换 Provider 的既有异常。
        provider_step.fail(TraceFailure::ProviderStreamFailed);
        throw;
    }
}

ProviderInfo DeepSeekProvider::info() const {
    ProviderInfo provider_info;
    provider_info.name = kDeepSeekProviderName;
    provider_info.default_model = config_.default_model.empty() ? kDeepSeekDefaultModel : config_.default_model;
    provider_info.supports_stream = true;
    provider_info.supports_tool_call = true;
    return provider_info;
}

}  // namespace aiSDK
