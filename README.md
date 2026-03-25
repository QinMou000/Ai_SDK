# AIChatServer & SDK

## 项目简介
这是一个基于 C++17 开发的智能聊天服务器与 SDK 项目。项目主要由两部分组成：
- **AI Chat SDK**：提供统一的 C++ 接口，支持多种大语言模型（DeepSeek、ChatGPT、Gemini 以及本地 Ollama 等），内置会话管理与基于 SQLite 的数据持久化功能。
- **AIChatServer**：基于 `cpp-httplib` 实现的 HTTP 服务端，提供 RESTful API 与精美的 Web UI，支持流式输出（SSE）、多会话隔离等功能。

## 主要特性
- 🚀 **多模型支持**：无缝对接 DeepSeek, ChatGPT, Gemini 以及本地 Ollama 模型。
- 💬 **会话管理**：支持多会话创建与切换，聊天记录通过 SQLite 本地持久化。
- ⚡ **流式输出**：支持 Server-Sent Events (SSE) 流式响应，提供丝滑的打字机输出体验。
- 🎨 **Web UI**：内置现代化响应式 Web 界面，支持 Markdown 渲染和代码高亮。
- ⚙️ **灵活配置**：支持通过配置文件（`.conf`）、命令行参数（`gflags`）和环境变量来加载服务配置及 API 密钥。

## 目录结构
```text
ai_sdk/
├── sdk/             # 核心 SDK 源码 (LLM 接入、会话与数据持久化管理)
├── ChatServer/      # HTTP 服务端源码及前端页面 (web 目录)
├── test/            # 基于 Google Test 的单元测试
└── README.md        # 项目说明文档
```

## 依赖说明
- **编译器**：支持 C++17
- **构建工具**：CMake 3.16+
- **第三方库**：
  - [spdlog](https://github.com/gabime/spdlog) & fmt - 高性能日志记录
  - [gflags](https://github.com/gflags/gflags) - 命令行参数解析
  - [OpenSSL](https://www.openssl.org/) - HTTPS/SSL 支持
  - [SQLite3](https://www.sqlite.org/index.html) - 本地数据持久化
  - [JsonCpp](https://github.com/open-source-parsers/jsoncpp) - JSON 数据解析
  - [cpp-httplib](https://github.com/yhirose/cpp-httplib) - 轻量级 HTTP 服务器 (CMake 将自动下载)
  - [GTest](https://github.com/google/googletest) - 单元测试框架

## 编译指南

### 编译 ChatServer
```bash
cd ChatServer
mkdir build && cd build
cmake ..
make -j4
```

### 编译并运行测试
```bash
cd test
mkdir build && cd build
cmake ..
make -j4
./test
```

## 配置与运行

### 1. 修改配置
编译服务端后，在 `ChatServer/build` 目录下会生成 `ChatServer.conf`。你可以编辑该文件配置你的 API Key：
```ini
server_addr=0.0.0.0
server_port=8080
log_level=INFO
temperature=0.7
max_tokens=2048

# 填入你的 API Key（也可以通过环境变量设置）
deepseek_api_key=your_deepseek_api_key_here
# chatgpt_api_key=your_chatgpt_api_key_here
# gemini_api_key=your_gemini_api_key_here
# ollama_endpoint=http://127.0.0.1:11434
```

### 2. 启动服务
```bash
./AIChatServer --config_file=ChatServer.conf
```
启动成功后，打开浏览器访问 `http://localhost:8080` 即可使用 Web 界面与 AI 助手进行对话。

---

## TODO List
- [ ] 完善更多本地模型的接入
- [ ] 优化前端会话列表交互体验
