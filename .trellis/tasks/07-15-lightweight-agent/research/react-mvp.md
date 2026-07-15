# 最小 ReAct Agent 研究

## 研究目标

确认最小 ReAct 循环的必要语义，并映射到当前 C++ SDK 已有的 Tool Call 接口，避免为了 Agent 修改 `AIClient` 或新增模型协议。

## 参考模式

### 原始 ReAct 模式

来源：[ReAct: Synergizing Reasoning and Acting in Language Models](https://arxiv.org/abs/2210.03629)

- ReAct 的核心是让模型推理与面向外部环境的动作交错进行。
- 动作返回的观察结果会进入后续推理，使模型能够更新计划、处理异常并决定下一步。
- 对本项目而言，不需要把私有推理文本暴露为新的公共协议；Tool Call 和后续模型调用已经能够形成行为等价的决策—动作—观察闭环。

### DeepSeek Tool Call 模式

来源：[DeepSeek Function Calling 文档](https://api-docs.deepseek.com/guides/function_calling/)、[DeepSeek Chat Completion 文档](https://api-docs.deepseek.com/api/create-chat-completion)

- 模型负责选择工具并生成工具名、参数和调用 ID，实际工具执行由应用完成。
- 应用需要先保留 assistant 的 Tool Call 消息，再追加带相同 `tool_call_id` 的 Tool 结果消息，之后重新调用模型。
- 工具参数可能不满足预期，执行前必须经过现有工具注册表与 Schema 校验边界。

### 当前仓库显式循环模式

来源：`examples/05_tool_call/main.cpp`、`src/tool/ToolExecutor.cpp`、`docs/PRD.md`

- 当前示例已经实现单轮 `LLM → Tool → LLM`，协议顺序与 DeepSeek 官方模式一致。
- `ToolExecutor` 会按模型返回顺序串行执行，并把未知工具或处理函数异常收敛为失败结果。
- 仓库路线图已经把 `SimpleAgent` 定义为该显式流程的有限循环版本，并把会话管理与记忆放到后续阶段。

## 映射到当前仓库

```text
用户任务
  → Reason：模型根据消息和工具 Schema 决策
  → Action：ChatResponse::tool_calls
  → Execute：AIClient::executeToolCalls 或 ToolExecutor::executeAll
  → Observation：ToolExecutionResult::toToolMessage
  → Reason：携带 assistant/tool 消息再次调用 AIClient::chat
  → Final：响应不再包含 Tool Call，返回最终文本
```

## 结论

- 首版应实现“ReAct 风格的有限 Tool Call 循环”，不单独设计 Thought/Action 文本解析器。
- 单个 `run(...)` 内维护临时消息轨迹；调用结束后不保留跨任务会话状态。
- 必须有最大步数，防止模型持续返回 Tool Call 造成无限循环与不可控成本。
- 工具失败应作为 Observation 回填，让模型获得一次自我修正机会；达到最大步数或模型调用抛出异常时再结束任务。
- 现有 `AIClient`、`Message`、`ChatResponse`、`ToolExecutor` 与 `TraceSession` 已覆盖全部集成点，无需修改核心客户端。
