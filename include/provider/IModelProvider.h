#pragma once

#include <string>
#include <utility>

#include "core/ChatRequest.h"
#include "core/ChatResponse.h"
#include "http/SSEParser.h"
#include "trace/TraceRecorder.h"

namespace aiSDK {

// ProviderInfo 描述单个模型提供方的静态能力。
// 上层可据此决定默认模型、是否启用流式输出，以及是否展示工具调用入口。
struct ProviderInfo {
    // name 是 Provider 的稳定标识，用于配置查找和运行时切换。
    std::string name;
    // default_model 在请求未显式指定模型时提供兜底值。
    std::string default_model;
    // supports_stream 标识当前 Provider 是否支持流式回调。
    bool supports_stream = true;
    // supports_tool_call 标识当前 Provider 是否支持工具调用协议。
    bool supports_tool_call = true;
};

// IModelProvider 定义统一模型接入边界。
// 所有具体 Provider 都必须把差异收敛到这一组接口下，避免泄漏到底层调用方。
class IModelProvider {
   public:
    virtual ~IModelProvider() = default;

    // chat 执行一次非流式请求，并返回统一的响应结构。
    virtual ChatResponse chat(const ChatRequest& request) = 0;
    // Trace 重载接收显式父步骤；默认实现保留第三方 Provider 的原调用契约。
    // 具体 Provider 可覆盖该入口，补充供应商和传输层子步骤。
    // 默认委托保证未适配 Trace 的实现仍可工作，但不会伪造无法观测的内部事件。
    virtual ChatResponse chat(const ChatRequest& request, TraceSession&, const std::string&) {
        return chat(request);
    }
    // streamChat 通过回调逐步产出结果，回调协议由 StreamEvent 定义。
    virtual void streamChat(const ChatRequest& request, StreamCallback callback) = 0;
    // 默认 Trace 实现不伪造底层步骤，只委托现有无 Trace 接口。
    // callback 只移动一次，保持原接口的所有权和异常传播约定。
    virtual void streamChat(const ChatRequest& request, StreamCallback callback, TraceSession&, const std::string&) {
        streamChat(request, std::move(callback));
    }
    // info 返回 Provider 的静态能力描述，供初始化和测试复用。
    virtual ProviderInfo info() const = 0;
};

}  // namespace aiSDK
