#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "core/Config.h"

namespace {

void setEnvValue(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

TEST(ConfigTest, ResolvesEnvironmentPlaceholders) {
    setEnvValue("AISDK_TEST_KEY", "resolved-value");

    const std::string resolved = aiSDK::resolveEnvPlaceholders("prefix-${AISDK_TEST_KEY}-suffix");

    EXPECT_EQ(resolved, "prefix-resolved-value-suffix");
}

TEST(ConfigTest, BuildsConfigFromJsonAndResolvesProviderSecrets) {
    setEnvValue("DEEPSEEK_API_KEY", "secret-from-env");

    const nlohmann::json json = {
        {"providers",
         {{"deepseek",
           {
               {"api_key", "${DEEPSEEK_API_KEY}"},
               {"base_url", "https://api.deepseek.com"},
               {"default_model", "deepseek-chat"},
           }}}},
        {"default_provider", "deepseek"},
        {"timeout_ms", 45000},
        {"enable_trace", true},
    };

    const aiSDK::Config config = aiSDK::configFromJson(json);

    const auto iterator = config.providers.find("deepseek");
    ASSERT_NE(iterator, config.providers.end());
    EXPECT_EQ(iterator->second.api_key, "secret-from-env");
    EXPECT_EQ(config.default_provider, "deepseek");
    EXPECT_EQ(config.timeout_ms, 45000);
    EXPECT_TRUE(config.enable_trace);
}

TEST(ConfigTest, LoadsConfigFromFile) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ai_sdk_config_test.json";
    std::ofstream output(path);
    output << R"({"default_provider":"minimax","timeout_ms":1200,"enable_trace":false})";
    output.close();

    const aiSDK::Config config = aiSDK::loadConfigFromFile(path);

    EXPECT_EQ(config.default_provider, "minimax");
    EXPECT_EQ(config.timeout_ms, 1200);
    EXPECT_FALSE(config.enable_trace);

    std::filesystem::remove(path);
}

TEST(ConfigTest, LoadsEnvFileAndResolvesProviderSecrets) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "ai_sdk_env_test";
    const std::filesystem::path env_path = directory / ".env";
    const std::filesystem::path config_path = directory / "config.json";

    std::filesystem::create_directories(directory);

    std::ofstream env_output(env_path);
    env_output << "DEEPSEEK_API_KEY=secret-from-dotenv\n";
    env_output.close();

    std::ofstream config_output(config_path);
    config_output << R"({
        "providers": {
            "deepseek": {
                "api_key": "${DEEPSEEK_API_KEY}",
                "base_url": "https://api.deepseek.com",
                "default_model": "deepseek-v4-flash"
            }
        }
    })";
    config_output.close();

    setEnvValue("DEEPSEEK_API_KEY", "");
    const aiSDK::Config config = aiSDK::loadConfigFromFile(config_path);

    const auto iterator = config.providers.find("deepseek");
    ASSERT_NE(iterator, config.providers.end());
    EXPECT_EQ(iterator->second.api_key, "secret-from-dotenv");

    std::filesystem::remove_all(directory);
}

}  // namespace
