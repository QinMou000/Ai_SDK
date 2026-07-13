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

    throw std::invalid_argument("unsupported provider: " + provider_name);
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

ChatResponse AIClient::chat(const ChatRequest& request) const {
    if(!active_provider_) {
        throw std::logic_error("active provider is not initialized");
    }

    return active_provider_->chat(request);
}

void AIClient::streamChat(const ChatRequest& request, StreamCallback callback) const {
    if(!active_provider_) {
        throw std::logic_error("active provider is not initialized");
    }

    active_provider_->streamChat(request, std::move(callback));
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

void AIClient::setProvider(const std::string& provider_name) {
    if(provider_name.empty()) {
        throw std::invalid_argument("provider_name must not be empty");
    }

    active_provider_ = createProvider(config_, provider_name);
    active_provider_name_ = provider_name;
}

}  // namespace aiSDK
