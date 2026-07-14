#include "http/HttpClient.h"

#include <cpr/cpr.h>

#include <exception>
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
// 这样普通请求和流式请求可以共享完全一致的网络参数。
// SSL 证书校验始终开启，调用方不能通过单次请求绕过安全校验。
void configureSession(cpr::Session& session, const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) {
    session.SetUrl(cpr::Url{url});
    session.SetHeader(toCprHeader(headers));
    session.SetBody(cpr::Body{body.dump()});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetVerifySsl(cpr::VerifySsl{true});
}

// throwIfTransportFailed 把底层网络错误提升为明确异常，
// 避免上层只看到空状态码而无法定位问题。
// HTTP 非 2xx 响应不属于传输失败，仍由 Provider 结合响应正文解释。
void throwIfTransportFailed(const cpr::Response& response, const std::string& url) {
    if(response.error) {
        throw std::runtime_error("HTTP 请求失败: " + url + " | " + response.error.message);
    }
}

}  // namespace

// 普通请求由 cpr 一次性收集正文，HttpClient 只做传输错误转换和结果归一化。
HttpResponse HttpClient::postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const {
    cpr::Session session;
    configureSession(session, url, body, headers, timeout_ms);
    const cpr::Response response = session.Post();
    throwIfTransportFailed(response, url);

    HttpResponse http_response;
    http_response.status_code = static_cast<int>(response.status_code);
    http_response.body = response.text;
    return http_response;
}

// 流式请求保持同步返回，但会在 session.Post() 执行期间把网络字节块及时交给上层。
HttpResponse HttpClient::postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                        HttpStreamCallback callback) const {
    cpr::Session session;
    configureSession(session, url, body, headers, timeout_ms);

    // 保留完整原始正文既满足返回契约，也便于 Provider 解释非 2xx 错误；回调异常则必须暂存，不能穿过 cpr 的 C 回调边界。
    std::string response_body;
    std::exception_ptr callback_error;
    session.SetWriteCallback(cpr::WriteCallback([&](std::string_view data, intptr_t) {
        // 先保存原始字节，再通知上层处理；空回调也不会影响正文收集。
        response_body.append(data);
        if(!callback) {
            return true;
        }

        try {
            callback(data);
            return true;
        } catch(...) {
            // false 会通知 cpr 中止后续读取，原始异常则留到 Post 返回后重抛。
            callback_error = std::current_exception();
            return false;
        }
    }));

    const cpr::Response response = session.Post();
    if(callback_error) {
        // 优先恢复上层解析异常，避免 cpr 的“回调中止”传输错误掩盖真实原因。
        std::rethrow_exception(callback_error);
    }

    throwIfTransportFailed(response, url);

    HttpResponse http_response;
    http_response.status_code = static_cast<int>(response.status_code);
    http_response.body = std::move(response_body);
    return http_response;
}

}  // namespace aiSDK
