#include <gflags/gflags.h>
#include <spdlog/common.h>

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include "ChatServer.h"

// 定义自定义标志
DEFINE_bool(h, false, "显示帮助信息");
DEFINE_bool(v, false, "显示版本信息");

namespace Ai_Chat_Server {
}  // namespace Ai_Chat_Server

int main(int argc, char** argv) {
    // 设置gflags帮助信息
    std::string usage = "AIChatServer - 智能聊天服务器\n用法: ";
    usage += argv[0];
    usage += " [选项]";
    gflags::SetUsageMessage(usage);

    // 解析命令行参数
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // 处理-h/--help
    if (FLAGS_h || FLAGS_help) {
        Ai_Chat_Server::printHelp(argv[0]);
        return 0;
    }

    // 处理-v/--version
    if (FLAGS_v || FLAGS_version) {
        Ai_Chat_Server::printVersion();
        return 0;
    }

    // 创建服务器配置
    Ai_Chat_Server::ServerConfig config;

    // 从配置文件加载（如果指定）
    if (!FLAGS_config_file.empty()) {
        if (!Ai_Chat_Server::parseConfigFile(FLAGS_config_file, config)) {
            std::cerr << "配置文件加载失败，使用命令行参数" << std::endl;
        }
    }

    // 命令行参数覆盖配置文件
    if (!FLAGS_server_addr.empty()) {
        config._serverAddr = FLAGS_server_addr;
    }
    if (FLAGS_server_port > 0) {
        config._serverPort = FLAGS_server_port;
    }
    if (!FLAGS_log_level.empty()) {
        config._logLevel = FLAGS_log_level;
    }
    if (FLAGS_temperature > 0) {
        config._temperature = FLAGS_temperature;
    }
    if (FLAGS_max_tokens > 0) {
        config._maxTokens = FLAGS_max_tokens;
    }

    // 从环境变量加载API密钥
    Ai_Chat_Server::loadApiKeysFromEnv(config);

    // 显示启动信息
    std::cout << "======================================" << std::endl;
    std::cout << "  AIChatServer 启动中..." << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "服务器地址: " << config._serverAddr << std::endl;
    std::cout << "服务器端口: " << config._serverPort << std::endl;
    std::cout << "日志级别: " << config._logLevel << std::endl;
    std::cout << "温度值: " << config._temperature << std::endl;
    std::cout << "最大Token: " << config._maxTokens << std::endl;
    std::cout << "DeepSeek API: " << (config._deepseekApiKey.empty() ? "未配置" : "已配置") << std::endl;
    std::cout << "ChatGPT API: " << (config._chatgptApiKey.empty() ? "未配置" : "已配置") << std::endl;
    std::cout << "Gemini API: " << (config._geminiApiKey.empty() ? "未配置" : "已配置") << std::endl;
    std::cout << "Ollama: " << (config._ollamaEndpoint.empty() ? "未配置" : config._ollamaEndpoint) << std::endl;
    std::cout << "======================================" << std::endl;

    // 创建并初始化服务器
    auto server = std::make_unique<Ai_Chat_Server::ChatServer>();

    if (!server->init(config)) {
        std::cerr << "服务器初始化失败" << std::endl;
        return 1;
    }

    // 启动服务器
    if (!server->start()) {
        std::cerr << "服务器启动失败" << std::endl;
        return 1;
    }

    std::cout << "服务器已启动，访问 http://" << config._serverAddr << ":" << config._serverPort << " 查看前端页面" << std::endl;
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;

    // 等待服务器停止
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
