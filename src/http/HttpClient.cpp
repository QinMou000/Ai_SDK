#include "http/HttpClient.h"

#include <cpr/cpr.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace aiSDK {
namespace {

// toCprHeader 负责把 SDK 内部请求头映射到 cpr 的 Header 类型。
cpr::Header toCprHeader(const HttpHeaders& headers) {
    cpr::Header cpr_headers;
    for(const auto& [key, value] : headers) {
        cpr_headers[key] = value;
    }
    return cpr_headers;
}

// configureSession 统一配置 URL、请求头、请求体和超时参数。
// 这样普通请求和流式请求可以共享完全一致的网络参数。
void configureSession(cpr::Session& session, const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) {
    session.SetUrl(cpr::Url{url});
    session.SetHeader(toCprHeader(headers));
    session.SetBody(cpr::Body{body.dump()});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetVerifySsl(cpr::VerifySsl{true});
}

// throwIfTransportFailed 把底层网络错误提升为明确异常，
// 避免上层只看到空状态码而无法定位问题。
void throwIfTransportFailed(const cpr::Response& response, const std::string& url) {
    if(response.error) {
        throw std::runtime_error("HTTP 请求失败: " + url + " | " + response.error.message);
    }
}

}  // namespace

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

HttpResponse HttpClient::postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                        HttpStreamCallback callback) const {
    cpr::Session session;
    configureSession(session, url, body, headers, timeout_ms);

    std::string response_body;
    std::exception_ptr callback_error;
    session.SetWriteCallback(cpr::WriteCallback([&](std::string_view data, intptr_t) {
        response_body.append(data);
        if(!callback) {
            return true;
        }

        try {
            callback(data);
            return true;
        } catch(...) {
            // 记录解析阶段异常，并通过中断回调尽快停止网络读取。
            callback_error = std::current_exception();
            return false;
        }
    }));

    const cpr::Response response = session.Post();
    if(callback_error) {
        std::rethrow_exception(callback_error);
    }

    throwIfTransportFailed(response, url);

    HttpResponse http_response;
    http_response.status_code = static_cast<int>(response.status_code);
    http_response.body = std::move(response_body);
    return http_response;
}

}  // namespace aiSDK
