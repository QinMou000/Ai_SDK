#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

#include "trace/TraceRecorder.h"

namespace aiSDK {

// HttpHeaders 统一承载 Provider 侧需要传递的请求头。
// 先使用简单字符串映射，避免在头文件层暴露 cpr 具体类型。
using HttpHeaders = std::unordered_map<std::string, std::string>;

// HttpStreamCallback 在同步网络读取期间消费流式响应的原始字节块。
// chunk 只在本次回调调用期间有效；需要跨回调保存时必须主动复制。
// 上层会基于这些字节块继续做缓冲、SSE 事件拆分和协议解析。
using HttpStreamCallback = std::function<void(std::string_view chunk)>;

struct HttpResponse {
    // status_code 保存 HTTP 状态码，便于上层统一判断 2xx / 非 2xx 结果。
    int status_code = 0;
    // body 保存响应正文；流式模式下会累积完整的原始 SSE 文本。
    std::string body;
};

// IHttpTransport 是 HttpClient 与具体网络库之间的窄边界。
// 生产实现使用 cpr，测试可注入确定性传输而不启动远程服务或 CI。
class IHttpTransport {
   public:
    virtual ~IHttpTransport() = default;

    // 传输实现只负责真实读写与底层错误转换，不记录 Trace。
    virtual HttpResponse postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const = 0;
    // 流式实现必须保持 callback 异常的原始传播语义。
    virtual HttpResponse postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                        HttpStreamCallback callback) const = 0;
};

// HttpClient 封装 Provider 当前所需的最小 HTTP 能力。
// 当前先聚焦 JSON POST 和流式 JSON POST，后续再按需要扩展更多方法。
class HttpClient {
   public:
    // 未注入 transport 时创建默认 cpr 实现；传入空指针同样回退到默认实现。
    explicit HttpClient(std::shared_ptr<IHttpTransport> transport = nullptr);

    // postJson 同步发送普通 JSON POST 请求，并返回状态码与完整响应正文。
    // 网络传输失败会抛出异常，非 2xx 状态码保留给 Provider 按协议处理。
    HttpResponse postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms) const;
    // Trace 重载只保存方法、模式、超时、状态码和字节数等白名单元数据。
    // URL、请求头和请求体不会进入 Trace，避免泄露查询参数与鉴权信息。
    HttpResponse postJson(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms, TraceSession& trace_session,
                          const std::string& parent_step_id) const;

    // postJsonStream 同步发送流式 JSON POST 请求，并在读取期间逐块调用 callback。
    // 网络分块不保证对应完整 SSE 事件，事件缓冲与协议解析由 Provider 负责。
    // callback 可以为空；返回值仍会包含已接收的完整原始正文。
    // callback 抛出异常时会停止后续读取，并在网络调用返回后原样重新抛出。
    // 网络传输失败会抛出异常，非 2xx 状态码和原始正文保留给 Provider 处理。
    HttpResponse postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms,
                                HttpStreamCallback callback) const;
    // 流式 Trace 与普通请求使用相同错误隔离规则；callback 异常仍原样重抛。
    HttpResponse postJsonStream(const std::string& url, const nlohmann::json& body, const HttpHeaders& headers, int timeout_ms, HttpStreamCallback callback,
                                TraceSession& trace_session, const std::string& parent_step_id) const;

   private:
    // shared_ptr 让 HttpClient 保持廉价可复制，Provider 测试也可共享同一假传输状态。
    std::shared_ptr<IHttpTransport> transport_;
};

}  // namespace aiSDK
