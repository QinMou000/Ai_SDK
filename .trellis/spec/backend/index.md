# AI SDK 后端规格索引

## 适用范围

本层覆盖当前仓库中所有已落地的 C++ SDK 代码与验证约定，主要对应：

- `include/`：对外公开头文件与接口契约。
- `src/`：核心实现，包括 `core`、`provider`、`http`、`tool`、`trace` 与 `AIClient`。
- `tests/`：GTest 单元测试与冒烟测试。
- `examples/`：面向使用者的最小可运行示例。

当前仓库没有前端源码目录、前端包清单或前端构建入口，因此 `.trellis/spec/` 只保留后端层和通用思考指南。

## 架构速览

- 构建入口在 `CMakeLists.txt` 与 `CMakePresets.json`，统一使用 C++17。
- SDK 公开入口是 `include/AIClient.h` 与 `src/AIClient.cpp`。
- 模型能力通过 `include/provider/IModelProvider.h` 抽象，目前唯一落地实现是 `src/provider/DeepSeekProvider.cpp`。
- HTTP 与流式解析由 `src/http/HttpClient.cpp`、`src/http/SSEParser.cpp` 承担。
- Tool Call 本地执行由 `include/tool/`、`src/tool/` 和 `AIClient::executeToolCalls(...)` 承担。
- 显式链路追踪由 `include/trace/`、`src/trace/` 和 `AIClient::startTrace()` 承担。
- 配置、消息、请求、响应模型位于 `include/core/` 与 `src/core/`。

## 开发前检查

- 先读 [目录结构规范](./directory-structure.md)，确认改动应该落在 `include/`、`src/`、`tests/` 还是 `examples/`。
- 如果改动触及 `AIClient`、Provider、HTTP/SSE 之间的数据边界，必须同时读 [SDK 契约](./sdk-contracts.md) 和 [错误处理](./error-handling.md)。
- 如果改动触及工具注册、Tool Call 批量执行或结果消息转换，也必须同时读 [SDK 契约](./sdk-contracts.md) 和 [错误处理](./error-handling.md)。
- 如果改动涉及调试、诊断或日志输出，先读 [日志规范](./logging-guidelines.md)。
- 如果改动触及 Trace 会话、步骤、脱敏或任一跨层埋点，必须读 [Trace 契约](./trace-contracts.md)。
- 如果改动引入持久化、缓存或外部存储能力，先读 [数据库与持久化规范](./database-guidelines.md)。

## 质量检查

- 代码必须与 `CMakeLists.txt` 的模块边界保持一致，避免把实现细节泄露到公开头文件。
- 本地验证优先使用仓库内真实预设：
  - `cmake --preset windows-debug`
  - `cmake --build --preset windows-debug`
  - `ctest --preset windows-debug --output-on-failure`
  - Linux 对应 `linux-debug`
- 只改文档时，至少执行规格一致性检查：无模板占位符、索引链接有效、规格目录结构与仓库事实一致。

## 主题导航

- [目录结构规范](./directory-structure.md)：哪些代码应该放在什么目录，以及公开头文件与实现文件如何配合。
- [SDK 契约](./sdk-contracts.md)：跨模块请求、响应、流式回调、配置和错误边界。
- [错误处理](./error-handling.md)：异常类型、抛出时机、流式解析错误与调用方感知方式。
- [日志规范](./logging-guidelines.md)：当前日志现状、允许的诊断方式、后续扩展约束。
- [Trace 契约](./trace-contracts.md)：显式会话、步骤层级、线程安全、默认白名单与脱敏边界。
- [数据库与持久化规范](./database-guidelines.md)：当前无数据库时的约束，以及未来引入持久化时的落点。
- [质量规范](./quality-guidelines.md)：测试、示例、构建预设与本地校验要求。
