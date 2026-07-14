#include "http/HttpClient.h"

#include <cpr/cpr.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace aiSDK {
namespace {

// toCprHeader 负责把 SDK 内部请求头映射到 cpr 的 Header 类型。
// 转换被限制在实现文件内，避免 Provider 和公共接口依赖底层网络库。
cpr::Header toCprHeader(const HttpHeaders& headers) {
    cpr::Header cpr_headers;
    for(const auto& [key, value] : headers) {
        cpr_headers[key] = value;
    }
    return cpr_headers;
}

// configureSession 统一配置 URL、请求头、请求体和超时参数。
// 普通请求和流式请求共享同一网络参数，SSL 证书校验始终开启。
void configureSession(cpr::Session& session, const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) {
    session.SetUrl(cpr::Url{url});
    session.SetHeader(toCprHeader(headers));
    session.SetBody(cpr::Body{body.dump()});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetVerifySsl(cpr::VerifySsl{true});
}

// throwIfTransportFailed 把底层网络错误提升为现有公开异常。
// HTTP 非 2xx 不属于传输失败，仍由 Provider 结合响应正文解释。
void throwIfTransportFailed(const cpr::Response& response, const std::string& url) {
    if(response.error) {
        throw std::runtime_error("HTTP 请求失败: " + url + " | " + response.error.message);
    }
}

// CprHttpTransport 是默认生产传输，所有 cpr 类型都被封闭在实现文件中。
// Trace 由外层 HttpClient 统一记录，因此替换传输不会改变可观测字段契约。
class CprHttpTransport final : public IHttpTransport {
   public:
    // 该适配器只负责执行网络 I/O 和归一化响应，不承担 Provider 语义判断。
    // 调用方可注入等价实现用于本地测试，而无需启动真实 HTTP 服务。
    // 适配器自身不接触 Trace，确保测试传输与生产传输拥有一致的观测外壳。
    HttpResponse postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const override {
        // 会话按请求创建，避免在并发调用之间共享回调、正文或认证状态。
        // 所有安全相关配置仍集中经过 configureSession，防止两种请求模式漂移。
        cpr::Session session;
        configureSession(session, url, body, headers, timeout_ms);
        const cpr::Response response = session.Post();
        throwIfTransportFailed(response, url);

        // 返回值只暴露 SDK 的稳定传输结构，不泄漏 cpr 的类型和生命周期。
        // 非 2xx 状态在这里保持原样，交由外层 Trace 和 Provider 分层解释。
        HttpResponse http_response;
        http_response.status_code = static_cast<int>(response.status_code);
        http_response.body = response.text;
        return http_response;
    }

    HttpResponse postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                HttpStreamCallback callback) const override {
        // 流式会话同样是单次调用对象，回调捕获的局部状态仅在 Post 期间有效。
        // 同步返回契约保证引用捕获不会越过本函数的作用域。
        cpr::Session session;
        configureSession(session, url, body, headers, timeout_ms);

        // 正文需要完整保留给 Provider；回调异常不能穿过 cpr 的 C 回调边界。
        // 先累计正文可确保上层即使不注册回调，仍能获得完整的 HTTP 响应。
        // exception_ptr 只承担跨越回调 ABI 边界的暂存职责，不改写异常类型。
        std::string response_body;
        std::exception_ptr callback_error;
        session.SetWriteCallback(cpr::WriteCallback([&](std::string_view data, intptr_t) {
            try {
                // 正文累积和上层回调都不能把异常带出 cpr 回调边界。
                // 此处不解析 SSE，传输层对正文协议保持无感知。
                response_body.append(data);
                if(callback) {
                    // 上层回调在 cpr 要求的同步窗口内执行，顺序与网络分块保持一致。
                    callback(data);
                }
                return true;
            } catch(...) {
                // false 中止后续读取，原异常在 Post 返回后优先恢复。
                // 这里禁止抛出，否则异常穿越第三方回调边界会产生未定义行为。
                callback_error = std::current_exception();
                return false;
            }
        }));

        const cpr::Response response = session.Post();
        if(callback_error) {
            // 保持原接口优先级，不能让 cpr 的“回调中止”错误覆盖调用方异常。
            // 恢复点位于 cpr 调用栈之外，因此调用方仍能按原类型捕获异常。
            std::rethrow_exception(callback_error);
        }
        // 仅当回调正常完成时才解释网络错误，维持清晰的失败责任优先级。
        throwIfTransportFailed(response, url);

        // 流式响应与普通响应使用同一归一化结构，Provider 无需依赖传输模式分支。
        HttpResponse http_response;
        http_response.status_code = static_cast<int>(response.status_code);
        http_response.body = std::move(response_body);
        return http_response;
    }
};

}  // namespace

// 传输依赖在构造期固定，保证单次请求期间不会发生实现切换。
// 空注入回退到生产实现，使默认构造与显式注入共享同一成员访问路径。
// HttpClient 仍负责 Trace 契约，因此自定义传输无法绕过字段白名单。
HttpClient::HttpClient(std::shared_ptr<IHttpTransport> transport) : transport_(transport ? std::move(transport) : std::make_shared<CprHttpTransport>()) {}

// 普通无 Trace 请求复用同一包装逻辑，禁用句柄不会产生任何步骤分配。
HttpResponse HttpClient::postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const {
    TraceSession disabled_trace;
    return postJson(url, body, headers, timeout_ms, disabled_trace, "");
}

HttpResponse HttpClient::postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms, TraceSession& trace_session,
                                  const std::string& parent_step_id) const {
    // HTTP 步骤只描述本层 I/O；父标识由 Provider 传入以维持稳定的调用树。
    // 禁用或创建失败时，Scope 的空操作语义保证业务请求仍可继续执行。
    TraceRecorder recorder(trace_session);
    TraceStepScope http_step = recorder.startStep(TraceStepType::HttpRequest, parent_step_id);
    if(http_step.enabled()) {
        // 固定白名单刻意不包含 URL、请求头和请求体。
        // 方法、模式和超时属于低敏元数据，可用于定位重试与配置问题。
        // 枚举键阻止调用点临时写入未审核字段。
        http_step.setAttribute(TraceAttributeKey::HttpMethod, "POST");
        http_step.setAttribute(TraceAttributeKey::Stream, false);
        http_step.setAttribute(TraceAttributeKey::TimeoutMs, timeout_ms);
    }

    try {
        // Trace 包装位于注入接口之外，生产与测试传输产生相同的步骤结构。
        HttpResponse response = transport_->postJson(url, body, headers, timeout_ms);
        if(http_step.enabled()) {
            http_step.setAttribute(TraceAttributeKey::HttpStatusCode, response.status_code);
            http_step.setAttribute(TraceAttributeKey::ResponseBytes, response.body.size());
        }
        if(response.status_code >= 200 && response.status_code < 300) {
            http_step.succeed();
        } else {
            // 非 2xx 仍返回给 Provider 解释，但 Trace 明确标记 HTTP 层结果。
            // 此处只写稳定机器码，服务端正文不会进入可观测属性。
            http_step.fail(TraceFailure::HttpStatusFailed);
        }
        return response;
    } catch(...) {
        // 底层异常可能含 URL 或网络库文本，Trace 不复制其内容。
        // 原异常继续抛给 Provider，观测逻辑不改变公开错误语义。
        http_step.fail(TraceFailure::HttpTransportFailed);
        throw;
    }
}

// 流式无 Trace 请求仍走相同回调和异常恢复实现。
HttpResponse HttpClient::postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                        HttpStreamCallback callback) const {
    TraceSession disabled_trace;
    return postJsonStream(url, body, headers, timeout_ms, std::move(callback), disabled_trace, "");
}

HttpResponse HttpClient::postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                        HttpStreamCallback callback, TraceSession& trace_session, const std::string& parent_step_id) const {
    // 流式步骤与普通请求使用相同类型，通过白名单属性区分模式。
    // 计数状态限定在同步调用内，避免跨请求共享产生竞态。
    TraceRecorder recorder(trace_session);
    TraceStepScope http_step = recorder.startStep(TraceStepType::HttpRequest, parent_step_id);
    if(http_step.enabled()) {
        // 流式模式只记录传输元数据，不保存原始分块内容。
        // 回调是否存在属于控制流事实，不暴露回调对象或捕获内容。
        http_step.setAttribute(TraceAttributeKey::HttpMethod, "POST");
        http_step.setAttribute(TraceAttributeKey::Stream, true);
        http_step.setAttribute(TraceAttributeKey::TimeoutMs, timeout_ms);
        http_step.setAttribute(TraceAttributeKey::CallbackPresent, static_cast<bool>(callback));
    }

    // 禁用会话或步骤创建失败时直接复用原传输路径，不为计数包装器分配内存。
    // 该分支保证关闭 Trace 后的流式回调层级与原实现完全一致。
    if(!http_step.enabled()) {
        return transport_->postJsonStream(url, body, headers, timeout_ms, std::move(callback));
    }

    std::size_t chunk_count = 0;
    std::size_t response_bytes = 0;
    bool callback_failed = false;
    // 包装回调只维护安全计数和失败来源，不缓存任何分块数据。
    // 无回调时保持空对象，使自定义传输仍能识别调用方未订阅增量。
    HttpStreamCallback counted_callback;
    if(callback) {
        // 计数包装器在转发前累计元数据；用户回调异常仍由传输实现原样传播。
        // 先计数可保留导致异常的最后一个分块，便于定位中断位置。
        try {
            counted_callback = [&](std::string_view chunk) {
                ++chunk_count;
                response_bytes += chunk.size();
                try {
                    callback(chunk);
                } catch(...) {
                    // 仅记录安全分类，底层仍负责恢复并原样抛出调用方异常。
                    // 布尔标记只区分责任边界，不保存异常文本或动态类型。
                    callback_failed = true;
                    throw;
                }
            };
        } catch(...) {
            // 包装器分配失败只关闭旁路步骤，真实传输仍使用原回调继续执行。
            http_step.fail(TraceFailure::TraceRecordingFailed);
            return transport_->postJsonStream(url, body, headers, timeout_ms, std::move(callback));
        }
    }

    try {
        // 移交包装回调后，传输实现负责在返回前结束全部回调活动。
        HttpResponse response = transport_->postJsonStream(url, body, headers, timeout_ms, std::move(counted_callback));
        // 空回调或自定义传输未逐块回调时，仍以最终正文长度补齐字节元数据。
        // 仅取更大值可避免既回调又返回正文的实现发生重复累计。
        if(response.body.size() > response_bytes) {
            response_bytes = response.body.size();
        }
        if(http_step.enabled()) {
            // 状态码、分块数和字节数均属于固定白名单，不包含正文片段。
            http_step.setAttribute(TraceAttributeKey::HttpStatusCode, response.status_code);
            http_step.setAttribute(TraceAttributeKey::ChunkCount, chunk_count);
            http_step.setAttribute(TraceAttributeKey::ResponseBytes, response_bytes);
        }
        if(response.status_code >= 200 && response.status_code < 300) {
            http_step.succeed();
        } else {
            http_step.fail(TraceFailure::HttpStatusFailed);
        }
        return response;
    } catch(...) {
        // 异常路径同样写入截至失败时的计数，保留部分传输的可诊断性。
        if(http_step.enabled()) {
            http_step.setAttribute(TraceAttributeKey::ChunkCount, chunk_count);
            http_step.setAttribute(TraceAttributeKey::ResponseBytes, response_bytes);
        }
        // 下游回调异常与网络传输异常采用不同机器码，便于定位责任边界。
        // 分类不会吞掉异常，调用方仍按既有方式决定重试或终止。
        http_step.fail(callback_failed ? TraceFailure::StreamCallbackFailed : TraceFailure::HttpStreamFailed);
        throw;
    }
}

}  // namespace aiSDK
