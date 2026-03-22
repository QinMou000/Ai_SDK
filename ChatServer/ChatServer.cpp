#include "ChatServer.h"

#include <csignal>
#include <sstream>
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <libgen.h>
#include <unistd.h>
#endif

#include "../sdk/include/ChatGPTProvider.h"
#include "../sdk/include/DeepSeekProvider.h"
#include "../sdk/include/GeminiProvider.h"
#include "../sdk/include/LocalLLMProvider.h"

#include <json/json.h>

// 跨平台路径分隔符
#ifdef _WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

// 定义命令行参数
DEFINE_string(server_addr, DEFAULT_ADDR, "服务器绑定地址");
DEFINE_int32(server_port, DEFAULT_PORT, "服务器端口号");
DEFINE_string(log_level, DEFAULT_LOG_LEVEL, "日志级别 (DEBUG, INFO, WARN, ERROR)");
DEFINE_double(temperature, DEFAULT_TEMPERATURE, "默认温度值 (0.0-2.0)");
DEFINE_int32(max_tokens, DEFAULT_MAX_TOKENS, "默认最大token数");
DEFINE_string(config_file, "", "配置文件路径");

namespace Ai_Chat_Server {

// 静态变量
static std::atomic<bool> g_shutdown(false);

ChatServer::ChatServer()
    : _server(nullptr), _running(false) {
    // 设置信号处理
    std::signal(SIGINT, [](int) { g_shutdown = true; });
    std::signal(SIGTERM, [](int) { g_shutdown = true; });
}

ChatServer::~ChatServer() {
    stop();
}

bool ChatServer::init(const ServerConfig& config) {
    _config = config;

    // 初始化日志
    spdlog::level::level_enum level = spdlog::level::info;
    if (config._logLevel == "DEBUG") {
        level = spdlog::level::debug;
    } else if (config._logLevel == "WARN" || config._logLevel == "WARNING") {
        level = spdlog::level::warn;
    } else if (config._logLevel == "ERROR") {
        level = spdlog::level::err;
    }
    Util::Logger::initLogger("ChatServer", "stdout", level);

    // 初始化SessionManager
    _sessionManager = std::make_unique<Ai_Chat_SDK::SessionManager>("chat.db");

    // 初始化LLMManager
    _llmManager = std::make_unique<Ai_Chat_SDK::LLMManager>();

    // 初始化模型
    if (!initModels()) {
        ERROR("初始化模型失败");
        return false;
    }

    // 创建HTTP服务器
    _server = std::make_unique<httplib::Server>();

    // 设置路由
    setupRoutes();

    INFO("ChatServer 初始化完成，监听 {}:{}", config._serverAddr, config._serverPort);
    return true;
}

bool ChatServer::initModels() {
    // 注册DeepSeek模型
    if (!_config._deepseekApiKey.empty()) {
        auto deepseek = std::make_unique<Ai_Chat_SDK::DeepSeekProvider>();
        std::map<std::string, std::string> config = {{"api_key", _config._deepseekApiKey}, {"end_point", "https://api.deepseek.com"}};
        if (deepseek->initModel(config)) {
            _llmManager->registerModel("deepseek", std::move(deepseek));
            INFO("DeepSeek 模型注册成功");
        } else {
            WARN("DeepSeek 模型初始化失败");
        }
    }

    // 注册ChatGPT模型
    if (!_config._chatgptApiKey.empty()) {
        auto chatgpt = std::make_unique<Ai_Chat_SDK::ChatGPTProvider>();
        std::map<std::string, std::string> config = {{"api_key", _config._chatgptApiKey}, {"end_point", "https://api.openai.com"}};
        if (chatgpt->initModel(config)) {
            _llmManager->registerModel("chatgpt", std::move(chatgpt));
            INFO("ChatGPT 模型注册成功");
        } else {
            WARN("ChatGPT 模型初始化失败");
        }
    }

    // 注册Gemini模型
    if (!_config._geminiApiKey.empty()) {
        auto gemini = std::make_unique<Ai_Chat_SDK::GeminiProvider>();
        std::map<std::string, std::string> config = {{"api_key", _config._geminiApiKey}, {"end_point", "https://generativelanguage.googleapis.com"}};
        if (gemini->initModel(config)) {
            _llmManager->registerModel("gemini", std::move(gemini));
            INFO("Gemini 模型注册成功");
        } else {
            WARN("Gemini 模型初始化失败");
        }
    }

    // 注册Ollama本地模型
    if (!_config._ollamaEndpoint.empty() && !_config._ollamaModel.empty()) {
        auto ollama = std::make_unique<Ai_Chat_SDK::LocalLLMProvider>();
        std::map<std::string, std::string> config = {{"api_key", ""}, {"end_point", _config._ollamaEndpoint}};
        if (ollama->initModel(config)) {
            _llmManager->registerModel(_config._ollamaModel, std::move(ollama));
            INFO("Ollama 模型注册成功: {}", _config._ollamaModel);
        } else {
            WARN("Ollama 模型初始化失败");
        }
    }

    return true;
}

void ChatServer::setupRoutes() {
    // 获取会话列表
    _server->Get("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetSessions(req, res);
    });

    // 获取可用模型
    _server->Get("/api/models", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetModels(req, res);
    });

    // 创建新会话
    _server->Post("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
        handleCreateSession(req, res);
    });

    // 获取会话历史
    _server->Get(R"(/api/session/([^/]+)/history)", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetSessionHistory(req, res);
    });

    // 删除会话
    _server->Delete(R"(/api/session/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        handleDeleteSession(req, res);
    });

    // 发送消息 - 流式响应
    _server->Post("/api/message/async", [this](const httplib::Request& req, httplib::Response& res) {
        handleSendMessageStream(req, res);
    });

    // 前端页面
    _server->Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        handleStaticFile(req, res);
    });

    _server->Get("/index.html", [this](const httplib::Request& req, httplib::Response& res) {
        handleStaticFile(req, res);
    });

    // CSS文件
    _server->Get("/css/style.css", [this](const httplib::Request& req, httplib::Response& res) {
        handleStaticFile(req, res);
    });

    // JS文件
    _server->Get("/js/app.js", [this](const httplib::Request& req, httplib::Response& res) {
        handleStaticFile(req, res);
    });

    // 帮助信息
    _server->Get("/help", [this](const httplib::Request& req, httplib::Response& res) {
        handleHelp(req, res);
    });

    // 404处理
    _server->set_not_found_handler([](const httplib::Request& req, httplib::Response& res) {
        res.status = 404;
        res.set_content("{\"success\":false,\"message\":\"Not Found\"}", "application/json");
    });
}

void ChatServer::handleGetSessions(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(_mutex);

    Json::Value root;
    root["success"] = true;
    root["message"] = "获取会话列表成功";

    Json::Value sessions;
    auto sessionList = getSessionInfoList();

    for (const auto& info : sessionList) {
        Json::Value session;
        session["id"] = info.id;
        session["model"] = info.model;
        session["created_at"] = static_cast<Json::Int64>(info.created_at);
        session["updated_at"] = static_cast<Json::Int64>(info.updated_at);
        session["message_count"] = info.message_count;
        session["first_user_message"] = info.first_user_message;
        sessions.append(session);
    }

    root["data"] = sessions;

    Json::StreamWriterBuilder builder;
    res.set_content(Json::writeString(builder, root), "application/json");
}

void ChatServer::handleGetModels(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(_mutex);

    Json::Value root;
    root["success"] = true;
    root["message"] = "获取模型列表成功";

    Json::Value models;
    auto availableModels = getAvailableModels();

    for (const auto& model : availableModels) {
        Json::Value m;
        m["name"] = model._modelName;
        m["desc"] = model._modelDesc;
        models.append(m);
    }

    root["data"] = models;

    Json::StreamWriterBuilder builder;
    res.set_content(Json::writeString(builder, root), "application/json");
}

void ChatServer::handleCreateSession(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(_mutex);

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(req.body);
    std::string errs;

    Json::Value requestJson;
    if (!Json::parseFromStream(builder, stream, &requestJson, &errs)) {
        root["success"] = false;
        root["message"] = "请求参数解析失败";
        Json::StreamWriterBuilder writer;
        res.set_content(Json::writeString(writer, root), "application/json");
        return;
    }

    std::string modelName = requestJson.get("model", "").asString();
    if (modelName.empty()) {
        root["success"] = false;
        root["message"] = "模型名称不能为空";
        Json::StreamWriterBuilder writer;
        res.set_content(Json::writeString(writer, root), "application/json");
        return;
    }

    // 验证模型是否可用
    if (!_llmManager->isAvailable(modelName)) {
        root["success"] = false;
        root["message"] = "模型不可用: " + modelName;
        Json::StreamWriterBuilder writer;
        res.set_content(Json::writeString(writer, root), "application/json");
        return;
    }

    // 创建会话
    std::string sessionId = _sessionManager->createSession(modelName);

    root["success"] = true;
    root["message"] = "会话创建成功";
    Json::Value data;
    data["session_id"] = sessionId;
    data["model"] = modelName;
    root["data"] = data;

    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, root), "application/json");
}

void ChatServer::handleGetSessionHistory(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string sessionId = req.matches[1];

    auto session = _sessionManager->getSession(sessionId);
    if (!session) {
        Json::Value root;
        root["success"] = false;
        root["message"] = "会话不存在";
        Json::StreamWriterBuilder writer;
        res.set_content(Json::writeString(writer, root), "application/json");
        return;
    }

    auto history = _sessionManager->getHistroyMessages(sessionId);

    Json::Value root;
    root["success"] = true;
    root["message"] = "获取历史消息成功";

    Json::Value messages;
    for (const auto& msg : history) {
        Json::Value m;
        m["id"] = msg._messageId;
        m["role"] = msg._role;
        m["content"] = msg._content;
        m["timestamp"] = static_cast<Json::Int64>(msg._timestamp);
        messages.append(m);
    }

    root["data"] = messages;

    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, root), "application/json");
}

void ChatServer::handleDeleteSession(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string sessionId = req.matches[1];

    bool success = _sessionManager->deleteSession(sessionId);

    Json::Value root;
    if (success) {
        root["success"] = true;
        root["message"] = "会话删除成功";
    } else {
        root["success"] = false;
        root["message"] = "会话删除失败";
    }

    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, root), "application/json");
}

void ChatServer::handleSendMessageStream(const httplib::Request& req, httplib::Response& res) {
    // 设置SSE响应头
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Access-Control-Allow-Origin", "*");

    Json::CharReaderBuilder builder;
    std::istringstream stream(req.body);
    std::string errs;

    Json::Value requestJson;
    if (!Json::parseFromStream(builder, stream, &requestJson, &errs)) {
        res.set_content("data: {\"error\":\"请求参数解析失败\"}\n\ndata: [DONE]\n\n", "text/event-stream");
        return;
    }

    std::string sessionId = requestJson.get("session_id", "").asString();
    std::string message = requestJson.get("message", "").asString();

    if (sessionId.empty() || message.empty()) {
        res.set_content("data: {\"error\":\"参数不完整\"}\n\ndata: [DONE]\n\n", "text/event-stream");
        return;
    }

    // 获取会话
    auto session = _sessionManager->getSession(sessionId);
    if (!session) {
        res.set_content("data: {\"error\":\"会话不存在\"}\n\ndata: [DONE]\n\n", "text/event-stream");
        return;
    }

    // 添加用户消息
    Ai_Chat_SDK::Message userMsg("user", message);
    _sessionManager->addMessage(sessionId, userMsg);

    // 获取历史消息
    auto history = _sessionManager->getHistroyMessages(sessionId);

    // 准备请求参数
    std::map<std::string, std::string> requestParam;
    requestParam["temperature"] = std::to_string(_config._temperature);
    requestParam["max_tokens"] = std::to_string(_config._maxTokens);

    // 存储完整响应
    std::string fullResponse;

    // 流式回调
    auto callback = [&res, &fullResponse](const std::string& content, bool isFinish) {
        fullResponse += content;
        std::string sseData = "data: " + content + "\n\n";
        res.write(sseData.c_str(), sseData.size());

        // 刷新响应
        res.flush();
    };

    // 调用LLM
    std::string response = _llmManager->sendMessageStream(session->_modelName, history, requestParam, callback);

    // 添加助手消息
    Ai_Chat_SDK::Message assistantMsg("assistant", fullResponse);
    _sessionManager->addMessage(sessionId, assistantMsg);

    // 发送完成信号
    res.write("data: [DONE]\n\n", 14);
}

void ChatServer::handleHelp(const httplib::Request& req, httplib::Response& res) {
    std::string help = R"(# AI Chat Server 帮助文档

## 使用说明

### 启动服务器
./AIChatServer [--server_addr=地址] [--server_port=端口] [--log_level=级别] [--temperature=温度] [--max_tokens=最大令牌数] [--config_file=配置文件]

### 参数说明
- server_addr: 服务器绑定地址 (默认: 0.0.0.0)
- server_port: 服务器端口号 (默认: 8080)
- log_level: 日志级别 (DEBUG, INFO, WARN, ERROR)
- temperature: 温度值 (默认: 0.7, 范围: 0.0-2.0)
- max_tokens: 最大令牌数 (默认: 2048)
- config_file: 配置文件路径

### API接口

1. 获取会话列表
   GET /api/sessions

2. 获取可用模型
   GET /api/models

3. 创建新会话
   POST /api/session
   Body: {"model": "模型名称"}

4. 获取会话历史
   GET /api/session/{session_id}/history

5. 发送消息 (流式)
   POST /api/message/async
   Body: {"session_id": "会话ID", "message": "消息内容"}

6. 删除会话
   DELETE /api/session/{session_id}

### 前端页面
访问 http://localhost:8080/ 查看聊天界面
)";

    res.set_content(help, "text/plain");
}

void ChatServer::handleStaticFile(const httplib::Request& req, httplib::Response& res) {
    std::string path = req.path;
    if (path == "/" || path == "/index.html") {
        path = "/index.html";
    }

    std::string filePath = _webRoot + path;

    // 尝试读取文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        res.status = 404;
        res.set_content("Not Found", "text/plain");
        return;
    }

    // 读取文件内容
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // 设置Content-Type
    std::string contentType = "text/plain";
    if (path.find(".html") != std::string::npos) {
        contentType = "text/html";
    } else if (path.find(".css") != std::string::npos) {
        contentType = "text/css";
    } else if (path.find(".js") != std::string::npos) {
        contentType = "application/javascript";
    } else if (path.find(".json") != std::string::npos) {
        contentType = "application/json";
    } else if (path.find(".png") != std::string::npos) {
        contentType = "image/png";
    } else if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) {
        contentType = "image/jpeg";
    }

    res.set_content(content, contentType);
}

std::vector<SessionInfo> ChatServer::getSessionInfoList() const {
    std::vector<SessionInfo> result;

    auto sessionIds = _sessionManager->getSessionLists();

    for (const auto& sessionId : sessionIds) {
        auto session = _sessionManager->getSession(sessionId);
        if (!session) continue;

        SessionInfo info;
        info.id = session->_sessionId;
        info.model = session->_modelName;
        info.created_at = static_cast<int64_t>(session->_createAt);
        info.updated_at = static_cast<int64_t>(session->_updateAt);
        info.message_count = static_cast<int>(session->_messages.size());

        // 获取第一条用户消息
        for (const auto& msg : session->_messages) {
            if (msg._role == "user") {
                info.first_user_message = msg._content;
                if (info.first_user_message.length() > 50) {
                    info.first_user_message = info.first_user_message.substr(0, 50) + "...";
                }
                break;
            }
        }

        result.push_back(info);
    }

    return result;
}

std::vector<Ai_Chat_SDK::LLMInfo> ChatServer::getAvailableModels() const {
    return _llmManager->getAvailableModels();
}

bool ChatServer::validateConfig() const {
    // 验证温度值
    if (_config._temperature < 0.0 || _config._temperature > 2.0) {
        ERROR("温度值必须在0.0-2.0之间，当前值: {}", _config._temperature);
        return false;
    }

    // 验证最大token数
    if (_config._maxTokens <= 0) {
        ERROR("最大token数必须大于0，当前值: {}", _config._maxTokens);
        return false;
    }

    // 验证至少有一个可用的模型配置
    bool hasValidModel = false;

    if (!_config._deepseekApiKey.empty()) hasValidModel = true;
    if (!_config._chatgptApiKey.empty()) hasValidModel = true;
    if (!_config._geminiApiKey.empty()) hasValidModel = true;
    if (!_config._ollamaEndpoint.empty() && !_config._ollamaModel.empty()) hasValidModel = true;

    if (!hasValidModel) {
        ERROR("至少需要配置一个有效的模型API密钥或Ollama配置");
        return false;
    }

    // 验证Ollama配置（如果配置了ollama，则所有参数都不能为空）
    if (!_config._ollamaEndpoint.empty() || !_config._ollamaModel.empty()) {
        if (_config._ollamaEndpoint.empty() || _config._ollamaModel.empty()) {
            ERROR("Ollama配置不完整，endpoint和model都不能为空");
            return false;
        }
    }

    return true;
}

bool ChatServer::start() {
    if (_running) {
        WARN("服务器已经在运行");
        return false;
    }

    if (!validateConfig()) {
        ERROR("配置验证失败");
        return false;
    }

    // 设置web根目录
    char exePath[1024] = {0};
#ifdef _WIN32
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
#else
    readlink("/proc/self/exe", exePath, sizeof(exePath));
#endif
    std::string exeDir = exePath;
    size_t pos = exeDir.find_last_of("/\\");
    if (pos != std::string::npos) {
        exeDir = exeDir.substr(0, pos);
    }
    _webRoot = exeDir + PATH_SEPARATOR + "web";

    INFO("Web根目录: {}", _webRoot);

    _running = true;

    // 启动服务器
    _server->listen(_config._serverAddr, _config._serverPort);

    return true;
}

void ChatServer::stop() {
    if (!_running) {
        return;
    }

    _running = false;

    if (_server) {
        _server->stop();
    }

    INFO("ChatServer 已停止");
}

bool parseConfigFile(const std::string& configFile, ServerConfig& config) {
    std::ifstream file(configFile);
    if (!file) {
        WARN("无法打开配置文件: {}", configFile);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // 跳过空行和注释
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // 解析 key=value 格式
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // 去除空白字符
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "server_addr") {
            config._serverAddr = value;
        } else if (key == "server_port") {
            config._serverPort = std::stoi(value);
        } else if (key == "log_level") {
            config._logLevel = value;
        } else if (key == "temperature") {
            config._temperature = std::stod(value);
        } else if (key == "max_tokens") {
            config._maxTokens = std::stoi(value);
        } else if (key == "deepseek_api_key") {
            config._deepseekApiKey = value;
        } else if (key == "chatgpt_api_key") {
            config._chatgptApiKey = value;
        } else if (key == "gemini_api_key") {
            config._geminiApiKey = value;
        } else if (key == "ollama_endpoint") {
            config._ollamaEndpoint = value;
        } else if (key == "ollama_model") {
            config._ollamaModel = value;
        }
    }

    return true;
}

void loadApiKeysFromEnv(ServerConfig& config) {
    // 从环境变量加载API密钥
    const char* envValue = std::getenv("DEEPSEEK_API_KEY");
    if (envValue) {
        config._deepseekApiKey = envValue;
    }

    envValue = std::getenv("CHATGPT_API_KEY");
    if (envValue) {
        config._chatgptApiKey = envValue;
    }

    envValue = std::getenv("GEMINI_API_KEY");
    if (envValue) {
        config._geminiApiKey = envValue;
    }

    envValue = std::getenv("OLLAMA_ENDPOINT");
    if (envValue) {
        config._ollamaEndpoint = envValue;
    }

    envValue = std::getenv("OLLAMA_MODEL");
    if (envValue) {
        config._ollamaModel = envValue;
    }
}

void printHelp(const char* progName) {
    std::cout << "用法: " << progName << " [选项]\n\n";
    std::cout << "选项:\n";
    std::cout << "  --server_addr=<地址>     服务器绑定地址 (默认: 0.0.0.0)\n";
    std::cout << "  --server_port=<端口>    服务器端口号 (默认: 8080)\n";
    std::cout << "  --log_level=<级别>      日志级别: DEBUG, INFO, WARN, ERROR (默认: INFO)\n";
    std::cout << "  --temperature=<值>      温度值 0.0-2.0 (默认: 0.7)\n";
    std::cout << "  --max_tokens=<数量>     最大token数 (默认: 2048)\n";
    std::cout << "  --config_file=<文件>    配置文件路径\n";
    std::cout << "  -h, --help              显示此帮助信息\n";
    std::cout << "  -v, --version           显示版本信息\n\n";

    std::cout << "环境变量:\n";
    std::cout << "  DEEPSEEK_API_KEY        DeepSeek API密钥\n";
    std::cout << "  CHATGPT_API_KEY         ChatGPT API密钥\n";
    std::cout << "  GEMINI_API_KEY          Gemini API密钥\n";
    std::cout << "  OLLAMA_ENDPOINT         Ollama服务端点\n";
    std::cout << "  OLLAMA_MODEL            Ollama模型名称\n\n";

    std::cout << "示例:\n";
    std::cout << "  " << progName << " --server_port=8080\n";
    std::cout << "  " << progName << " --config_file=ChatServer.conf\n";
    std::cout << "  " << progName << " --temperature=0.8 --max_tokens=4096\n\n";

    std::cout << "API接口:\n";
    std::cout << "  GET  /api/sessions                 获取会话列表\n";
    std::cout << "  GET  /api/models                   获取可用模型\n";
    std::cout << "  POST /api/session                   创建新会话\n";
    std::cout << "  GET  /api/session/{id}/history     获取会话历史\n";
    std::cout << "  POST /api/message/async             发送消息(流式)\n";
    std::cout << "  DELETE /api/session/{id}           删除会话\n";
}

void printVersion() {
    std::cout << "AIChatServer 版本 1.0.0\n";
}

}  // namespace Ai_Chat_Server
