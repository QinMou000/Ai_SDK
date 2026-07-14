#pragma once

#include "core/Config.h"
#include "http/HttpClient.h"
#include "http/SSEParser.h"
#include "provider/IModelProvider.h"

namespace aiSDK {

// DeepSeekProvider 负责把统一请求映射到 DeepSeek 的 OpenAI-compatible API。
// 当前实现已经直接发起真实 HTTP 请求，并负责解析普通响应与流式响应。
class DeepSeekProvider : public IModelProvider {
   public:
    // config 保存当前 Provider 的密钥、地址和默认模型配置。
    // timeout_ms 使用顶层配置中的统一超时设置，默认与 Config 保持一致。
    explicit DeepSeekProvider(ProviderConfig config, int timeout_ms = 30000);
    // 可注入 HttpClient 的重载用于本地确定性测试和自定义传输。
    // Provider 仍只依赖 HttpClient 契约，不接触具体 cpr 类型。
    DeepSeekProvider(ProviderConfig config, int timeout_ms, HttpClient http_client);

    // chat 发起一次真实的非流式 Chat Completion 请求。
    ChatResponse chat(const ChatRequest& request) override;
    // Trace 重载记录 Provider、HTTP 与统一模型请求之间的显式父子关系。
    ChatResponse chat(const ChatRequest& request, TraceSession& trace_session, const std::string& parent_step_id) override;
    // streamChat 发起真实的流式请求，并按 SSE 事件持续回调增量结果。
    void streamChat(const ChatRequest& request, StreamCallback callback) override;
    // 流式 Trace 只记录聚合计数，不保存逐 token 文本或原始 SSE 字节。
    void streamChat(const ChatRequest& request, StreamCallback callback, TraceSession& trace_session, const std::string& parent_step_id) override;
    // info 暴露 DeepSeek 的名称、默认模型和能力标识。
    ProviderInfo info() const override;

   private:
    // config_ 持有当前 Provider 的静态配置，避免每次请求重复读取外部状态。
    ProviderConfig config_;
    // timeout_ms_ 统一控制普通请求和流式请求的网络超时。
    int timeout_ms_ = 30000;
    // http_client_ 负责执行底层 HTTP 请求。
    HttpClient http_client_;
    // sse_parser_ 负责把 SSE 文本块拆成统一流式事件。
    SSEParser sse_parser_;
};

}  // namespace aiSDK
