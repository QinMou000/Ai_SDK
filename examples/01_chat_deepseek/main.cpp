#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "AIClient.h"

namespace {

std::string requireEnv(const char* key) {
    const std::string value = aiSDK::getEnvValue(key);
    if(value.empty()) {
        throw std::runtime_error(std::string("缺少环境变量: ") + key);
    }
    return value;
}

std::string getEnvOrDefault(const char* key, const char* fallback) {
    const std::string value = aiSDK::getEnvValue(key);
    if(value.empty()) {
        return fallback;
    }
    return value;
}

std::string joinPrompt(int argc, char** argv) {
    if(argc <= 1) {
        return "请用三句话介绍 DeepSeek API SDK 的用途。";
    }

    std::ostringstream prompt;
    for(int index = 1; index < argc; ++index) {
        if(index > 1) {
            prompt << ' ';
        }
        prompt << argv[index];
    }
    return prompt.str();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // 示例启动时自动向上查找 .env，方便从仓库根目录或 build 子目录直接运行。
        aiSDK::loadNearestEnvFile(std::filesystem::current_path());

        aiSDK::Config config;
        config.default_provider = "deepseek";
        config.providers["deepseek"] = aiSDK::ProviderConfig{
            requireEnv("DEEPSEEK_API_KEY"),
            getEnvOrDefault("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
            getEnvOrDefault("DEEPSEEK_MODEL", "deepseek-v4-flash"),
        };

        aiSDK::AIClient client(config);

        aiSDK::ChatRequest request;
        request.messages.push_back(aiSDK::SystemMessage("你是一个简洁、准确的中文助手。"));
        request.messages.push_back(aiSDK::UserMessage(joinPrompt(argc, argv)));

        const aiSDK::ChatResponse response = client.chat(request);
        std::cout << response.content << std::endl;
        return 0;
    } catch(const std::exception& exception) {
        std::cerr << "DeepSeek 示例执行失败: " << exception.what() << std::endl;
        return 1;
    }
}
