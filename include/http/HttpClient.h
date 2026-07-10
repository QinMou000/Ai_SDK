#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aiSDK {

// HttpHeaders 统一承载 Provider 侧需要传递的请求头。
// 先使用简单字符串映射，避免在头文件层暴露 cpr 具体类型。
using HttpHeaders = std::unordered_map<std::string, std::string>;

// HttpStreamCallback 负责消费流式响应的原始字节块。
// 上层会基于这些字节块继续做 SSE 事件拆分和协议解析。
using HttpStreamCallback = std::function<void(std::string_view chunk)>;

struct HttpResponse {
    // status_code 保存 HTTP 状态码，便于上层统一判断 2xx / 非 2xx 结果。
    int status_code = 0;
    // body 保存响应正文；流式模式下会累积完整的原始 SSE 文本。
    std::string body;
};

// HttpClient 封装 Provider 当前所需的最小 HTTP 能力。
// 当前先聚焦 JSON POST 和流式 JSON POST，后续再按需要扩展更多方法。
class HttpClient {
   public:
    // postJson 发送普通 JSON POST 请求，并返回状态码与响应正文。
    HttpResponse postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const;

    // postJsonStream 发送流式 JSON POST 请求。
    // callback 会按网络分块收到原始字节，完整协议解析由上层负责。
    HttpResponse postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                HttpStreamCallback callback) const;
};

}  // namespace aiSDK
