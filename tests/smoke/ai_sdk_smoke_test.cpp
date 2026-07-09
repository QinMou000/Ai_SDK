#include <stdexcept>

#include <gtest/gtest.h>

#include "AIClient.h"

namespace {

TEST(AIClientSmokeTest, SupportsDeepSeekAndRejectsUnsupportedProvider) {
    aiSDK::Config config;
    config.default_provider = "deepseek";

    aiSDK::AIClient client(config);
    EXPECT_NO_THROW(client.setProvider("deepseek"));
    EXPECT_THROW(client.setProvider("minimax"), std::invalid_argument);
}

TEST(AIClientSmokeTest, RejectsEmptyProviderName) {
    aiSDK::Config config;
    config.default_provider = "deepseek";

    aiSDK::AIClient client(config);
    EXPECT_THROW(client.setProvider(""), std::invalid_argument);
}

}  // namespace
