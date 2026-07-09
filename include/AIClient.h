#pragma once

#include <memory>
#include <string>

#include "core/ChatRequest.h"
#include "core/ChatResponse.h"
#include "core/Config.h"
#include "http/SSEParser.h"
#include "provider/IModelProvider.h"
#include "tool/ToolRegistry.h"

namespace aiSDK {

// AIClient 是 SDK 的统一入口。
// 当前阶段先稳定配置管理、Provider 选择和基础请求委托链路。
class AIClient {
   public:
    // config 允许按值传入，便于调用方使用临时配置快速构造客户端。
    explicit AIClient(Config config = {});

    // 这些访问器用于读取当前运行时状态，而不暴露可变内部对象。
    const Config& config() const;
    const std::string& activeProviderName() const;

    // chat / streamChat 将请求委托给当前激活的 Provider 执行。
    // 后续接入真实网络层时，上层调用方式无需变化。
    ChatResponse chat(const ChatRequest& request) const;
    void streamChat(const ChatRequest& request, StreamCallback callback) const;

    // tools 暴露本地工具注册表，供后续 ToolCall 链路复用。
    ToolRegistry& tools();
    const ToolRegistry& tools() const;

    // setProvider 切换当前激活的 Provider，并立即绑定对应实现。
    void setProvider(const std::string& provider_name);

   private:
    Config config_;
    std::string active_provider_name_;
    ToolRegistry tool_registry_;
    // active_provider_ 保存当前生效的 Provider 实例，避免每次请求重复构造。
    std::shared_ptr<IModelProvider> active_provider_;
};

}  // namespace aiSDK
