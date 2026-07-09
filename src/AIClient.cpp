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
        return std::make_shared<DeepSeekProvider>(
            findProviderConfig(config, provider_name), config.timeout_ms);
    }

    throw std::invalid_argument("unsupported provider: " + provider_name);
}

}  // namespace

AIClient::AIClient(Config config)
    : config_(std::move(config)), active_provider_name_(config_.default_provider) {
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

void AIClient::setProvider(const std::string& provider_name) {
    if(provider_name.empty()) {
        throw std::invalid_argument("provider_name must not be empty");
    }

    active_provider_ = createProvider(config_, provider_name);
    active_provider_name_ = provider_name;
}

}  // namespace aiSDK
