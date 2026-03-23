#pragma once

#include <gflags/gflags.h>
#include <httplib.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <json/json.h>

#include "../sdk/include/LLMManager.h"
#include "../sdk/include/SessionManager.h"
#include "../sdk/include/util/Log.h"

// 命令行参数定义
DECLARE_string(server_addr);
DECLARE_int32(server_port);
DECLARE_string(log_level);
DECLARE_double(temperature);
DECLARE_int32(max_tokens);
DECLARE_string(config_file);

// 默认值定义
constexpr int DEFAULT_PORT = 8080;
constexpr const char* DEFAULT_ADDR = "0.0.0.0";
constexpr const char* DEFAULT_LOG_LEVEL = "INFO";
constexpr double DEFAULT_TEMPERATURE = 0.7;
constexpr int DEFAULT_MAX_TOKENS = 2048;
constexpr int MAX_MESSAGE_LENGTH = 2000;

namespace Ai_Chat_Server {

// 服务器配置
struct ServerConfig {
    std::string _serverAddr;
    int _serverPort;
    std::string _logLevel;
    double _temperature;
    int _maxTokens;
    std::string _deepseekApiKey;
    std::string _chatgptApiKey;
    std::string _geminiApiKey;
    std::string _ollamaEndpoint;
    std::string _ollamaModel;

    ServerConfig()
        : _serverAddr(DEFAULT_ADDR),
          _serverPort(DEFAULT_PORT),
          _logLevel(DEFAULT_LOG_LEVEL),
          _temperature(DEFAULT_TEMPERATURE),
          _maxTokens(DEFAULT_MAX_TOKENS) {}
};

// 响应结构
struct ApiResponse {
    bool success;
    std::string message;
    Json::Value data;

    Json::Value toJson() const {
        Json::Value root;
        root["success"] = success;
        root["message"] = message;
        root["data"] = data;
        return root;
    }
};

// 会话信息（包含message_count和first_user_message）
struct SessionInfo {
    std::string id;
    std::string model;
    int64_t created_at;
    int64_t updated_at;
    int message_count;
    std::string first_user_message;
};

class ChatServer {
   public:
    ChatServer();
    ~ChatServer();

    // 初始化服务器
    bool init(const ServerConfig& config);

    // 启动服务器
    bool start();

    // 停止服务器
    void stop();

    // 获取配置
    const ServerConfig& getConfig() const { return _config; }

    // 获取可用模型
    std::vector<Ai_Chat_SDK::LLMInfo> getAvailableModels() const;

   private:
    // 加载配置文件
    bool loadConfigFile(const std::string& configFile);

    // 初始化模型
    bool initModels();

    // 设置HTTP路由
    void setupRoutes();

    // API处理器
    void handleGetSessions(const httplib::Request& req, httplib::Response& res);
    void handleGetModels(const httplib::Request& req, httplib::Response& res);
    void handleCreateSession(const httplib::Request& req, httplib::Response& res);
    void handleGetSessionHistory(const httplib::Request& req, httplib::Response& res);
    void handleDeleteSession(const httplib::Request& req, httplib::Response& res);
    void handleSendMessage(const httplib::Request& req, httplib::Response& res);

    // SSE流式响应
    void handleSendMessageStream(const httplib::Request& req, httplib::Response& res);

    // 帮助信息
    void handleHelp(const httplib::Request& req, httplib::Response& res);

    // 静态文件服务（前端页面）
    void handleStaticFile(const httplib::Request& req, httplib::Response& res);

    // 获取会话列表详细信息
    std::vector<SessionInfo> getSessionInfoList() const;

    // 验证配置
    bool validateConfig() const;

   private:
    std::unique_ptr<httplib::Server> _server;
    ServerConfig _config;
    std::unique_ptr<Ai_Chat_SDK::SessionManager> _sessionManager;
    std::unique_ptr<Ai_Chat_SDK::LLMManager> _llmManager;
    std::atomic<bool> _running;
    std::mutex _mutex;

    // 前端资源路径
    std::string _webRoot;
};

// 显示帮助信息
void printHelp(const char* progName);

// 显示版本信息
void printVersion();

// 解析配置文件
bool parseConfigFile(const std::string& configFile, ServerConfig& config);

// 从环境变量获取API密钥
void loadApiKeysFromEnv(ServerConfig& config);

}  // namespace Ai_Chat_Server
