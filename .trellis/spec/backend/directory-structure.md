# 目录结构规范

## 适用范围

当你需要为 SDK 新增类型、Provider、HTTP 能力、示例或测试时，先按本文件决定落点，再开始编码。

## 当前目录职责

- `include/core/`：对外可见的数据模型与配置类型，例如 `Config`、`Message`、`ChatRequest`、`ChatResponse`。
- `include/provider/`：Provider 抽象和实现声明，例如 `IModelProvider`、`DeepSeekProvider`。
- `include/http/`：HTTP 与 SSE 的公开接口；这里可以暴露 SDK 自己的抽象，但不要暴露 `cpr` 细节。
- `include/tool/`：公开的工具定义、注册表、Tool Call 和批量执行接口；对应实现位于 `src/tool/`。
- `include/trace/`：公开 Trace 值类型、显式会话和步骤记录接口；对应实现位于 `src/trace/`。
- `include/agent/`：上层 Agent 公开接口与工作区文件工具选项；对应实现位于 `src/agent/`。
- `src/core/`：与 `include/core/` 一一对应的实现。
- `src/provider/`：模型供应商实现，目前只有 `DeepSeekProvider.cpp`。
- `src/http/`：HTTP 传输与 SSE 解析实现。
- `src/tool/`：工具结果构造、注册校验、异常收敛、稳定列举与串行批量执行。
- `src/trace/`：Trace JSON 序列化、线程安全共享状态、ID、时间和 RAII 步骤生命周期。
- `src/agent/`：无状态同步 ReAct 编排、工具风险筛选和受限工作区文本工具实现；不得把 Provider 或 HTTP 细节移入该层。
- `src/AIClient.cpp`：SDK 门面，负责 Provider 选择、工具注册入口与对外聊天接口。
- `examples/`：按场景拆分的示例入口，展示真实调用方式而不是测试替身。
- `tests/`：GTest 测试；当前分为 `core`、`provider`、`http`、`tool`、`trace`、`agent`、`smoke`。

## 本仓库遵循的结构规则

### 1. 公开接口先放 `include/`

- 任何会被示例、测试或 SDK 使用者直接包含的声明，都应先定义在 `include/`。
- `src/` 中不要再声明第二套平行接口；实现只依附公开声明。
- 参考：`include/http/HttpClient.h` 只暴露 `HttpHeaders`、`HttpResponse`、`IHttpTransport` 和 `HttpClient`，具体 `cpr::Session` 被封装在 `src/http/HttpClient.cpp`。

### 2. 头文件保持库级抽象，三方细节留在实现里

- 不要把 `cpr`、具体 JSON 解析技巧或临时网络状态对象直接放进公开头文件。
- 参考：`include/http/HttpClient.h` 没有出现 `cpr::Session`；`src/http/HttpClient.cpp` 里再完成 `cpr` 配置。
- 这样能降低 ABI 泄漏风险，也让 Provider 与上层只依赖 SDK 自己的类型。

### 3. 一个实现目录对应一个职责层

- `src/core/` 只做模型序列化、配置装载、值对象逻辑。
- `src/provider/` 负责供应商请求拼装、鉴权、错误转换。
- `src/http/` 负责传输、流式回调与 SSE 事件切分。
- 新功能如果同时跨越两层以上，优先补充边界类型或小型辅助函数，不要把所有逻辑塞进 `AIClient.cpp`。

### 4. 测试目录跟随生产代码职责，而不是跟随文件名

- 配置与消息模型测试放在 `tests/core/`。
- Provider 协议测试放在 `tests/provider/`。
- 协议解析和网络边界测试放在 `tests/http/`。
- 跨模块最小通路验证放在 `tests/smoke/`。
- Trace 并发、脱敏和跨层确定性验证放在 `tests/trace/`。
- Agent 循环、低风险策略和工作区文件边界放在 `tests/agent/`；其中 Provider 必须使用脚本化注入，文件必须使用系统临时目录。
- 如果新增模块没有测试目录，先判断它属于哪一层，复用对应测试分层；只有形成独立职责时才新建测试子目录。

## 当前未落地但已预留的目录

- `include/tool/` 已有对应 `src/tool/` 实现和 `tests/tool/ai_sdk_tool_test`；后续扩展必须继续保持公开声明、实现和测试分层一致。
- `include/trace/` 已有对应 `src/trace/`、`tests/trace/ai_sdk_trace_test` 和离线示例；扩展时必须同步维护 Trace 契约。
- `include/agent/` 已有 `src/agent/`、`tests/agent/ai_sdk_agent_test` 和 `examples/06_simple_agent`；后续会话、记忆、审批或 MCP 适配必须保持在该上层，不得侵入 `AIClient`。

## 常见错误

- 把第三方库类型直接暴露进公开头文件，导致上层与实现细节耦合。
- 把 `examples/` 当成临时实验区，写入与 SDK 公共接口不一致的调用方式。
- 在 `tests/` 下创建与生产层次无关的随意目录，导致定位测试责任困难。

## 新增文件时的落点判断

- 新增公开模型或配置：放 `include/core/` + `src/core/`。
- 新增供应商实现：放 `include/provider/` + `src/provider/`，并在 `AIClient.cpp` 的 Provider 创建逻辑补入口。
- 新增网络适配或协议解析：优先放 `include/http/` + `src/http/`。
- 新增 Trace 模型或记录策略：放 `include/trace/` + `src/trace/`，测试放 `tests/trace/`。
- 新增 Agent 编排或受限本地工具：放 `include/agent/` + `src/agent/`，测试放 `tests/agent/`，并更新 [Agent 契约](./agent-contracts.md)。
- 新增示例：放 `examples/<场景>/`，并在 `examples/CMakeLists.txt` 挂接。
- 新增测试：放对应 `tests/<layer>/`，并更新该层 `CMakeLists.txt`。
