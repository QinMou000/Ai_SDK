# AI SDK 产品需求文档 PRD v0.2

## 1. 项目名称

**AI SDK for C++**

可选名称：

**AI SDK：一个支持多模型、ToolCall、MCP、Skill 扩展的纯 C++ AI SDK**

## 2. 项目定位

本项目不是直接做一个聊天应用，也不是直接复刻 LangChain / LangGraph。

本项目第一阶段要做的是：

> 一个纯 C++ 实现的 AI SDK，向上为 Agent、ChatBot、AI 应用提供统一的大模型调用、流式输出、ToolCall、工具注册、MCP 接入和 Skill 加载能力。

也就是说：

* **SDK 层负责能力封装**
* **Agent 层负责思考、规划、循环、调用工具**
* **应用层负责具体业务场景**

项目分层如下：

```text
应用层：ChatBot / Agent Demo / Web UI / CLI
          ↓
Agent 层：Agent Loop / Planner / Memory / Skill Selector
          ↓
SDK 层：Model Client / ToolCall / MCP Client / Skill Loader / Trace
          ↓
基础层：HTTP / JSON / SSE / SQLite / Config / Logger
```

第一阶段重点是 **SDK**。
第二阶段再基于 SDK 做 **Agent Runtime**。

## 3. 为什么要重写

当前项目更像一个 ChatSDK，核心能力是：

```text
用户输入 → 调用模型 → 返回文本
```

但是目标项目应该是：

```text
用户输入 → 模型判断是否需要工具 → 生成 ToolCall → SDK 解析 ToolCall → 执行工具 → 回填工具结果 → 后续可由 Agent 继续决策
```

所以要从底层重新设计，而不是在原来的聊天逻辑上硬塞功能。

重写的核心原因：

1. **Chat 和 Agent 的抽象不同**

   * Chat 只需要 message in / message out。
   * Agent 需要 message、tool、memory、trace、state、loop。

2. **ToolCall 需要统一协议层**

   * 不同模型返回格式可能略有差异。
   * SDK 应该统一解析成自己的 `ToolCall` 结构。

3. **后续要接 MCP / Skill**

   * 如果前期没有设计扩展点，后面会很难加。

4. **纯 C++ 项目更有区分度**

   * 大多数 Agent 框架是 Python。
   * 纯 C++ 实现可以体现工程能力、架构设计和底层封装能力。

## 4. 一句话目标

**用纯 C++ 实现一个轻量级 AI SDK，先支持 DeepSeek 和 MiniMax 两个模型，提供 Chat、Stream、ToolCall、Tool Registry、MCP Client、Skill Loader 等基础能力，并为后续 Agent Runtime 提供底层支撑。**

## 5. 首期支持范围

### 5.1 模型支持

第一版只支持两个模型 Provider：

1. **DeepSeek**
2. **MiniMax**

原因：

* 两者都提供 OpenAI-compatible API，适合统一封装。DeepSeek 官方说明其 API 支持 OpenAI/Anthropic 兼容格式；MiniMax 也提供 OpenAI-compatible Chat Completion 接口。
* 两者都可以覆盖国产模型接入场景。
* 支持数量少，便于把 SDK 架构做扎实。
* 后续再扩展 OpenAI、Qwen、Ollama、OpenRouter 等 Provider。

### 5.2 不做的内容

第一阶段不做：

* 不做完整 Agent。
* 不做复杂多 Agent 协作。
* 不做 Workflow 可视化。
* 不做 RAG 全家桶。
* 不做向量数据库。
* 不做模型训练。
* 不做插件市场。
* 不做 Python Agent Server。
* 不做十几个模型 Provider。
* 不做复杂 LangGraph 状态图。

第一阶段只做 SDK 能力。

## 6. 产品分层

## 6.1 SDK Core

SDK Core 是整个项目的核心，负责通用抽象。

包括：

* 消息结构
* 模型请求结构
* 模型响应结构
* Provider 抽象
* ToolCall 抽象
* 错误结构
* Trace 结构
* 配置加载
* 日志系统

## 6.2 Model Provider

Provider 负责具体模型 API 适配。

第一版：

* DeepSeekProvider
* MiniMaxProvider

统一暴露：

```cpp
class IModelProvider {
public:
    virtual ChatResponse chat(const ChatRequest& request) = 0;
    virtual void streamChat(
        const ChatRequest& request,
        StreamCallback callback
    ) = 0;
    virtual ~IModelProvider() = default;
};
```

## 6.3 ToolCall Layer

ToolCall Layer 负责：

* 定义工具 Schema
* 把工具 Schema 注入模型请求
* 解析模型返回的工具调用
* 统一不同模型的 ToolCall 格式
* 把工具执行结果转换成模型可理解的 tool message

DeepSeek 官方 Tool Calls 文档说明，模型可以根据用户问题决定调用外部工具，并输出工具调用信息。MiniMax 的 Tool Use 文档也说明其模型会输出 `tool_calls`，其中包含函数名和参数。

## 6.4 Tool Registry

Tool Registry 负责管理本地工具。

包括：

* 注册工具
* 查询工具
* 启用 / 禁用工具
* 校验工具参数
* 执行工具
* 记录工具调用结果

第一版只支持 C++ 本地函数工具。

后续支持：

* HTTP 工具
* Shell 工具
* Python 脚本工具
* MCP 工具
* Skill 工具

## 6.5 MCP Client

MCP Client 作为 SDK 的扩展能力。

第一阶段可以先设计接口，不一定立刻完整实现。

目标：

* 连接外部 MCP Server
* 获取 MCP Server 提供的 tools
* 把 MCP tool 转换成本地统一 Tool
* 通过 SDK ToolCall 机制调用 MCP Tool

## 6.6 Skill Loader

Skill Loader 用于加载本地能力包。

Skill 不是 Agent 本身。
Skill 是 SDK 可以读取的一组工具、说明、资源和脚本。

示例：

```text
skills/
  code-review/
    SKILL.md
    tools.json
    resources/
      cpp-style-guide.md
    scripts/
      analyze.py
```

SDK 只负责加载和解析 Skill。
至于“什么时候用这个 Skill”，应该由后续 Agent 层决定。

## 6.7 Agent Layer

Agent Layer 不属于第一阶段 MVP，但要预留接口。

后续 Agent 会基于 SDK 实现：

* Agent Loop
* Planner
* Memory
* Tool Selection
* Skill Selection
* 多轮工具调用
* Trace 可视化

## 7. 核心用户故事

### 7.1 作为 C++ 开发者，我希望用统一接口调用 DeepSeek / MiniMax

```cpp
AIClient client(config);

ChatRequest request;
request.model = "deepseek-chat";
request.messages.push_back(UserMessage("你好，介绍一下你自己"));

auto response = client.chat(request);
std::cout << response.content << std::endl;
```

验收标准：

* 能切换 DeepSeek / MiniMax。
* 上层调用方式基本一致。
* API Key、Base URL、模型名可以配置。

## 7.2 作为开发者，我希望支持流式输出

```cpp
client.streamChat(request, [](const StreamEvent& event) {
    if (event.type == StreamEventType::Delta) {
        std::cout << event.delta;
    }
});
```

验收标准：

* 支持 SSE 流式解析。
* 支持增量文本输出。
* 支持流式结束事件。
* 支持错误事件。

## 7.3 作为开发者，我希望注册本地 C++ 工具

```cpp
Tool getTimeTool;
getTimeTool.name = "get_current_time";
getTimeTool.description = "Get current local time";
getTimeTool.parameters = JsonSchema::object({
    {"timezone", JsonSchema::string("Timezone name")}
});

registry.registerTool(getTimeTool, [](const Json& args) -> ToolResult {
    return ToolResult::success(getCurrentTime(args["timezone"]));
});
```

验收标准：

* 工具有名称、描述、参数 Schema。
* 工具可以注册到 Registry。
* 工具可以被查询。
* 工具可以被执行。

## 7.4 作为开发者，我希望模型可以产生 ToolCall

用户输入：

```text
现在东京几点？
```

SDK 请求模型时携带工具定义。

模型返回：

```json
{
  "tool_calls": [
    {
      "id": "call_001",
      "function": {
        "name": "get_current_time",
        "arguments": "{\"timezone\":\"Asia/Tokyo\"}"
      }
    }
  ]
}
```

SDK 解析为：

```cpp
ToolCall call;
call.id = "call_001";
call.name = "get_current_time";
call.arguments = Json{{"timezone", "Asia/Tokyo"}};
```

验收标准：

* 能解析模型返回的 ToolCall。
* 能校验工具名是否存在。
* 能校验参数是否符合 Schema。
* 能调用对应工具。
* 能生成 tool result message。

## 7.5 作为开发者，我希望 SDK 不直接变成 Agent

SDK 不应该自动无限循环。

SDK 只提供：

```text
模型调用
工具定义
ToolCall 解析
工具执行
工具结果封装
Trace 记录
```

至于下面这个循环：

```text
LLM → ToolCall → ToolResult → LLM → ToolCall → ToolResult → Final Answer
```

应该放在 Agent 层。

这样分层更清楚，也更适合后续扩展。

## 8. 功能需求

## 8.1 配置模块

### 功能描述

支持从配置文件加载模型、密钥、超时、日志、工具开关等信息。

### 配置示例

```json
{
  "providers": {
    "deepseek": {
      "api_key": "${DEEPSEEK_API_KEY}",
      "base_url": "https://api.deepseek.com",
      "default_model": "deepseek-chat"
    },
    "minimax": {
      "api_key": "${MINIMAX_API_KEY}",
      "base_url": "https://api.minimax.io",
      "default_model": "MiniMax-M2.5"
    }
  },
  "default_provider": "deepseek",
  "timeout_ms": 30000,
  "enable_trace": true
}
```

### 优先级

P0

### 验收标准

* 支持 JSON 配置文件。
* 支持环境变量读取 API Key。
* 支持默认 Provider。
* 支持请求超时配置。

## 8.2 Message 模块

### 功能描述

定义统一消息结构。

### 消息类型

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

```cpp
struct Message {
    Role role;
    std::string content;
    std::optional<std::string> name;
    std::optional<std::string> tool_call_id;
    std::vector<ToolCall> tool_calls;
};
```

### 优先级

P0

### 验收标准

* 支持 system / user / assistant / tool。
* 支持 assistant message 携带 tool_calls。
* 支持 tool message 绑定 tool_call_id。
* 能转换成 DeepSeek / MiniMax 各自 API 需要的 JSON。

## 8.3 Model Provider 模块

### 功能描述

封装不同模型供应商差异。

### Provider 接口

```cpp
class IModelProvider {
public:
    virtual ChatResponse chat(const ChatRequest& request) = 0;

    virtual void streamChat(
        const ChatRequest& request,
        std::function<void(const StreamEvent&)> callback
    ) = 0;

    virtual ProviderInfo info() const = 0;

    virtual ~IModelProvider() = default;
};
```

### DeepSeekProvider

能力：

* 普通 Chat
* Stream Chat
* ToolCall
* OpenAI-compatible 请求格式

### MiniMaxProvider

能力：

* 普通 Chat
* Stream Chat
* ToolCall
* OpenAI-compatible 请求格式

### 优先级

P0

### 验收标准

* 两个 Provider 都能通过统一接口调用。
* 支持错误解析。
* 支持 HTTP 状态码处理。
* 支持 JSON 解析失败处理。
* 支持超时处理。

## 8.4 Chat 模块

### 功能描述

提供最基本的同步聊天能力。

### 示例

```cpp
ChatRequest request;
request.messages = {
    SystemMessage("You are a helpful assistant."),
    UserMessage("解释一下 C++ RAII")
};

ChatResponse response = client.chat(request);
```

### 优先级

P0

### 验收标准

* 能完成一次普通问答。
* 能传入多轮历史。
* 能返回文本内容。
* 能返回 token usage，如果 Provider 支持。

## 8.5 Stream 模块

### 功能描述

提供 SSE 流式响应解析能力。

### StreamEvent 类型

```cpp
enum class StreamEventType {
    Delta,
    ToolCallDelta,
    Done,
    Error
};
```

### 优先级

P0

### 验收标准

* 能解析 SSE chunk。
* 能增量输出文本。
* 能识别结束事件。
* 能处理中途断流。
* 能在流式过程中收集完整响应。

## 8.6 Tool Schema 模块

### 功能描述

定义工具参数结构，并转换成模型 API 所需 JSON Schema。

### 示例

```cpp
ToolSchema schema;
schema.name = "get_weather";
schema.description = "Get weather by city";
schema.parameters = JsonSchema::object({
    {"city", JsonSchema::string("City name")}
});
```

### 优先级

P0

### 验收标准

* 支持 string / number / boolean / object / array。
* 支持 required 参数。
* 支持 description。
* 能转换为 DeepSeek / MiniMax 可接受的 tools 格式。

## 8.7 Tool Registry 模块

### 功能描述

管理本地 C++ 工具。

### 接口设计

```cpp
class ToolRegistry {
public:
    void registerTool(const Tool& tool, ToolHandler handler);

    bool hasTool(const std::string& name) const;

    Tool getTool(const std::string& name) const;

    std::vector<Tool> listTools() const;

    ToolResult execute(
        const std::string& name,
        const Json& arguments
    );
};
```

### 优先级

P0

### 验收标准

* 可以注册工具。
* 可以查询工具。
* 可以执行工具。
* 工具不存在时返回明确错误。
* 工具执行异常时返回明确错误。
* 工具支持超时配置。

## 8.8 ToolCall 解析模块

### 功能描述

将模型返回的工具调用解析成 SDK 内部统一结构。

### 内部结构

```cpp
struct ToolCall {
    std::string id;
    std::string name;
    Json arguments;
    std::string raw_arguments;
};
```

### 优先级

P0

### 验收标准

* 能解析 DeepSeek ToolCall。
* 能解析 MiniMax ToolCall。
* 能处理 arguments 是字符串 JSON 的情况。
* 能处理 arguments JSON 格式错误。
* 能保留原始 raw response，方便 Debug。

## 8.9 Tool Executor 模块

### 功能描述

执行模型请求的工具调用。

### 接口

```cpp
class ToolExecutor {
public:
    std::vector<ToolExecutionResult> executeAll(
        const std::vector<ToolCall>& calls
    );
};
```

### 第一版策略

第一版建议串行执行工具。

不做并行工具调用。

原因：

* 容易调试。
* Trace 更清楚。
* Agent 初期不需要复杂并发。

### 优先级

P0

### 验收标准

* 能执行单个 ToolCall。
* 能执行多个 ToolCall。
* 能生成 ToolResult Message。
* 能记录执行耗时。
* 能记录失败原因。

## 8.10 Trace 模块

### 功能描述

记录 SDK 内部关键事件。

### Trace 内容

* 请求开始
* 请求结束
* Provider
* Model
* HTTP 状态码
* LLM 响应耗时
* ToolCall 解析结果
* Tool 执行参数
* Tool 执行结果
* 错误信息

### 示例结构

```json
{
  "trace_id": "trace_001",
  "provider": "deepseek",
  "model": "deepseek-chat",
  "steps": [
    {
      "type": "llm_request",
      "timestamp": 123456,
      "duration_ms": 1200
    },
    {
      "type": "tool_call",
      "tool_name": "get_current_time",
      "duration_ms": 5,
      "status": "success"
    }
  ]
}
```

### 优先级

P1

### 验收标准

* 每次请求都有 trace_id。
* ToolCall 能看到完整链路。
* 支持导出 JSON。
* 支持关闭 Trace。

## 8.11 MCP Client 模块

### 功能描述

为后续接入 MCP Server 做准备。

第一版可以先做接口和最小 Demo。

### MVP 范围

* MCP Server 配置
* 启动本地 MCP Server
* 初始化连接
* 获取 tools/list
* 调用 tools/call
* 转换为 SDK Tool

### 优先级

P2

### 验收标准

* 能接一个最简单的 MCP Server。
* 能把 MCP tool 注册到 ToolRegistry。
* 能通过 SDK ToolExecutor 调用 MCP tool。

## 8.12 Skill Loader 模块

### 功能描述

加载本地 Skill 目录。

### 第一版 Skill 不负责自动决策

SDK 只做：

* 扫描 Skill
* 读取 SKILL.md
* 读取 tools.json
* 注册 Skill 中的工具
* 提供 Skill 描述给上层 Agent

### 目录结构

```text
skills/
  cpp-code-review/
    SKILL.md
    tools.json
    resources/
      style-guide.md
```

### 优先级

P2

### 验收标准

* 能扫描 skills 目录。
* 能读取 Skill 元信息。
* 能列出 Skill。
* 能把 Skill 里的工具注册到 ToolRegistry。
* 能被上层 Agent 查询。

## 9. SDK 和 Agent 的边界

这是本项目最重要的边界。

### 9.1 SDK 负责什么

SDK 负责“能力”。

包括：

* 调模型
* 流式输出
* 统一消息结构
* 统一 Provider
* 定义工具
* 注册工具
* 解析 ToolCall
* 执行工具
* 封装 ToolResult
* 加载 Skill
* 连接 MCP
* 记录 Trace

### 9.2 Agent 负责什么

Agent 负责“决策”。

包括：

* 是否调用工具
* 调用哪个工具
* 是否继续下一轮
* 是否加载某个 Skill
* 是否保存记忆
* 是否拆解任务
* 什么时候结束
* 失败后是否重试

### 9.3 SDK 不应该做什么

SDK 不应该：

* 自动无限循环调用模型
* 自动决定任务计划
* 自动读写长期记忆
* 自动选择 Skill
* 自动做复杂推理
* 自动替用户执行高风险操作

这些都应该放在 Agent 层。

## 10. 后续 Agent 设计预留

SDK 完成后，可以基于它做一个 Agent。

Agent 最小闭环：

```text
User Input
   ↓
Agent 构造 ChatRequest
   ↓
SDK 调用模型
   ↓
SDK 返回 AssistantMessage
   ↓
如果有 ToolCall
   ↓
SDK 执行 ToolCall
   ↓
Agent 把 ToolResult 加回 messages
   ↓
再次调用 SDK
   ↓
直到没有 ToolCall
   ↓
Final Answer
```

Agent 伪代码：

```cpp
AgentResult Agent::run(const std::string& input) {
    messages.push_back(UserMessage(input));

    for (int i = 0; i < max_steps; i++) {
        ChatRequest request;
        request.messages = messages;
        request.tools = toolRegistry.listTools();

        ChatResponse response = client.chat(request);
        messages.push_back(response.message);

        if (response.tool_calls.empty()) {
            return AgentResult::final(response.content);
        }

        auto results = toolExecutor.executeAll(response.tool_calls);

        for (auto& result : results) {
            messages.push_back(result.toToolMessage());
        }
    }

    return AgentResult::failed("Max steps exceeded");
}
```

## 11. 推荐开发路线

## v0.1：SDK Core + DeepSeek Chat

目标：

* 项目骨架重建。
* 支持 DeepSeek 普通 Chat。
* 支持 JSON 配置。
* 支持基础 Message 结构。
* 支持同步 HTTP 请求。

交付物：

* `AIClient`
* `DeepSeekProvider`
* `ChatRequest`
* `ChatResponse`
* README Demo

验收 Demo：

```cpp
AIClient client("config.json");
auto response = client.chat("你好，介绍一下你自己");
std::cout << response.content << std::endl;
```

## v0.2：MiniMax Provider + Stream

目标：

* 接入 MiniMax。
* 支持统一 Provider 切换。
* 支持 SSE 流式输出。

交付物：

* `MiniMaxProvider`
* `StreamParser`
* `StreamEvent`

验收 Demo：

```cpp
client.setProvider("minimax");
client.streamChat(request, [](auto event) {
    std::cout << event.delta;
});
```

## v0.3：Tool Schema + Tool Registry

目标：

* 支持本地 C++ 工具注册。
* 支持工具 Schema。
* 支持工具执行。

交付物：

* `Tool`
* `ToolSchema`
* `ToolRegistry`
* `ToolExecutor`

验收 Demo：

```cpp
registry.registerTool("get_current_time", ...);
auto result = registry.execute("get_current_time", args);
```

## v0.4：ToolCall Parsing

目标：

* 模型可以返回 ToolCall。
* SDK 可以解析 ToolCall。
* SDK 可以执行工具。
* SDK 可以生成 ToolResult Message。

交付物：

* `ToolCallParser`
* `ToolCall`
* `ToolResultMessage`

验收 Demo：

```text
用户：现在东京几点？
模型：调用 get_current_time
SDK：执行工具
SDK：返回 ToolResult Message
```

注意：这一版还不是 Agent，只是 SDK 具备 ToolCall 能力。

## v0.5：Simple Agent Demo

目标：

* 基于 SDK 做一个最小 Agent。
* 实现 LLM → Tool → LLM 的循环。

交付物：

* `SimpleAgent`
* `AgentExecutor`
* `max_steps`
* `AgentTrace`

验收 Demo：

```text
用户：现在东京几点？顺便根据时间说一句问候。
Agent：
1. 调用模型
2. 模型产生 get_current_time ToolCall
3. SDK 执行工具
4. Agent 回填工具结果
5. 再次调用模型
6. 输出最终答案
```

## v0.6：MCP Client

目标：

* 接入 MCP 工具。
* MCP Tool 可以注册到 SDK ToolRegistry。

交付物：

* `MCPClient`
* `MCPToolAdapter`
* `MCPServerConfig`

验收 Demo：

```text
用户：列出当前项目目录文件。
Agent 调用 MCP filesystem tool。
```

## v0.7：Skill Loader

目标：

* 支持本地 Skill。
* Skill 可以被 Agent 查询和加载。

交付物：

* `Skill`
* `SkillLoader`
* `SkillRegistry`

验收 Demo：

```text
用户：帮我审查这个 C++ 文件。
Agent 发现 cpp-code-review Skill。
Agent 加载 Skill 说明。
Agent 使用相关工具完成审查。
```

## 12. 推荐项目目录

```text
ai_sdk/
  CMakeLists.txt
  README.md
  docs/
    PRD.md
    ARCHITECTURE.md
    TOOLCALL.md
    PROVIDER.md
    MCP.md
    SKILL.md

  include/
      core/
        Message.h
        ChatRequest.h
        ChatResponse.h
        Error.h
        Result.h
        Config.h

      provider/
        IModelProvider.h
        DeepSeekProvider.h
        MiniMaxProvider.h

      http/
        HttpClient.h
        SSEParser.h

      tool/
        Tool.h
        ToolSchema.h
        ToolCall.h
        ToolRegistry.h
        ToolExecutor.h

      trace/
        Trace.h
        TraceRecorder.h

      mcp/
        MCPClient.h
        MCPToolAdapter.h

      skill/
        Skill.h
        SkillLoader.h
        SkillRegistry.h

      agent/
        SimpleAgent.h

      AIClient.h

  src/
    core/
    provider/
    http/
    tool/
    trace/
    mcp/
    skill/
    agent/

  examples/
    01_chat_deepseek/
    02_chat_minimax/
    03_stream_chat/
    04_register_tool/
    05_tool_call/
    06_simple_agent/
    07_mcp_demo/
    08_skill_demo/

  tests/
    core/
    provider/
    tool/
    agent/

  skills/
    cpp-code-review/
      SKILL.md
      tools.json
      resources/
```

## 13. 技术选型建议

### 13.1 C++ 标准

建议使用：

```text
C++17 起步，C++20 可选
```

理由：

* C++17 足够做 SDK。
* 面试接受度高。
* 生态兼容性好。
* 如果需要 coroutine，再考虑 C++20。

### 13.2 依赖库

建议：

| 能力     | 推荐              |
| ------ | --------------- |
| HTTP   | cpr / libcurl   |
| JSON   | nlohmann/json   |
| SSE    | 自己封装 parser     |
| 日志     | spdlog          |
| 配置     | nlohmann/json   |
| 测试     | GoogleTest      |
| 构建     | CMake           |
| SQLite | sqlite3，后续再加    |
| UUID   | stduuid 或自己简单生成 |

第一版不要引太多依赖。

### 13.3 命名空间

建议统一：

```cpp
namespace aiSDK {
}
```

## 14. 核心类设计草案

### 14.1 AIClient

```cpp
class AIClient {
public:
    explicit AIClient(const Config& config);

    ChatResponse chat(const ChatRequest& request);

    void streamChat(
        const ChatRequest& request,
        StreamCallback callback
    );

    ToolRegistry& tools();

    void setProvider(const std::string& provider_name);

private:
    std::unique_ptr<IModelProvider> provider_;
    ToolRegistry tool_registry_;
};
```

### 14.2 ChatRequest

```cpp
struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<Tool> tools;
    bool stream = false;
    double temperature = 0.7;
    int max_tokens = 2048;
};
```

### 14.3 ChatResponse

```cpp
struct ChatResponse {
    Message message;
    std::string content;
    std::vector<ToolCall> tool_calls;
    Usage usage;
    RawResponse raw;
};
```

### 14.4 Tool

```cpp
struct Tool {
    std::string name;
    std::string description;
    Json parameters;
    ToolRiskLevel risk_level = ToolRiskLevel::Low;
};
```

### 14.5 ToolResult

```cpp
struct ToolResult {
    bool success;
    Json data;
    std::string error_message;

    static ToolResult successResult(const Json& data);
    static ToolResult errorResult(const std::string& error);
};
```

## 15. 简历包装方向

这个项目后续可以包装成：

> 基于 C++17 设计并实现轻量级 AI SDK，支持 DeepSeek / MiniMax 多模型统一接入、SSE 流式响应、ToolCall 协议解析、本地工具注册与执行、MCP 工具扩展和 Skill 能力包加载；在 SDK 基础上实现 Simple Agent Demo，完成 LLM-Tool-LLM 多轮调用闭环，并通过 Trace 记录模型请求与工具调用链路，提升 Agent 行为可观测性和可调试性。

更短一点：

> 设计并实现纯 C++ AI SDK，封装 DeepSeek / MiniMax 模型接入、流式响应、ToolCall、工具注册、MCP 与 Skill 扩展能力，并基于 SDK 实现轻量 Agent 执行闭环。

## 16. 项目亮点

1. **纯 C++ 实现**

   * 和大量 Python Agent 框架形成差异。
   * 更适合 C++ / 后端 / AI Infra 简历方向。

2. **SDK 和 Agent 分层清楚**

   * SDK 负责能力。
   * Agent 负责决策。
   * 架构更专业。

3. **只支持两个 Provider，但抽象完整**

   * 不堆模型数量。
   * 重点做统一接口和扩展性。

4. **ToolCall 是核心突破点**

   * 从 ChatSDK 升级到 AgentSDK 的关键。

5. **MCP / Skill 预留生态扩展**

   * 后续有想象空间。
   * 面试时能讲架构演进。

6. **Trace 可观测**

   * 能展示模型请求、工具调用、错误和耗时。
   * 比单纯聊天项目更工程化。

## 17. 最小可交付版本

最小版本不需要 MCP，不需要 Skill，不需要完整 Agent。

真正的 MVP 是：

```text
DeepSeek Provider
MiniMax Provider
Chat
Stream
Tool Registry
ToolCall Parser
Tool Executor
Simple ToolCall Demo
```

也就是：

> 用户问问题，模型返回 ToolCall，SDK 解析并执行本地 C++ 工具，生成工具结果。

只要这个跑通，项目就已经不是普通 ChatSDK 了。

## 18. 最推荐的第一阶段开发顺序

1. 重建项目目录和 CMake。
2. 写 `Message / ChatRequest / ChatResponse`。
3. 写 `IModelProvider`。
4. 实现 `DeepSeekProvider`。
5. 实现 `MiniMaxProvider`。
6. 实现普通 Chat。
7. 实现 SSE Stream。
8. 实现 `Tool / ToolSchema`。
9. 实现 `ToolRegistry`。
10. 实现 `ToolCallParser`。
11. 实现 `ToolExecutor`。
12. 做 `get_current_time` Demo。
13. 做 `calculator` Demo。
14. 做 `SimpleAgent` Demo。
15. 再做 MCP。
16. 再做 Skill。

## 19. 边界结论

最终边界如下：

```text
第一阶段：纯 C++ AI SDK
重点：模型接入、流式输出、ToolCall、工具注册、工具执行

第二阶段：基于 SDK 的 Simple Agent
重点：LLM-Tool-LLM 循环

第三阶段：MCP + Skill
重点：扩展外部工具和能力包

第四阶段：Memory + Planner + Trace UI
重点：接近 LangChain / LangGraph 的 Agent Runtime
```

一句话总结：

**先把 SDK 做成“模型 + 工具调用能力层”，再把 Agent 做成“基于 SDK 的决策执行层”。**
