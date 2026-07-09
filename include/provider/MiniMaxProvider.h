#pragma once

#include "core/Config.h"
#include "provider/IModelProvider.h"

namespace aiSDK {

// MiniMaxProvider 与 DeepSeekProvider 共享同一抽象边界，
// 这里先把后续扩展点预留出来。
class MiniMaxProvider : public IModelProvider {
   public:
    explicit MiniMaxProvider(ProviderConfig config);

    ChatResponse chat(const ChatRequest& request) override;
    void streamChat(const ChatRequest& request, StreamCallback callback) override;
    ProviderInfo info() const override;

   private:
    ProviderConfig config_;
};

}  // namespace aiSDK