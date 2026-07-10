# 跨层思考指南

这个仓库最容易出问题的地方，不是单个函数，而是跨越配置、Provider、HTTP 和 SSE 解析时的边界漂移。

## 当改动触及这些文件时，默认按跨层问题处理

- `src/AIClient.cpp`
- `src/provider/DeepSeekProvider.cpp`
- `src/http/HttpClient.cpp`
- `src/http/SSEParser.cpp`
- `src/core/Config.cpp`

## 进入实现前先核对

- 输入从哪一层进入：配置文件、环境变量、调用方 `ChatRequest`，还是网络响应？
- 谁拥有字段语义：`ChatRequest`、Provider 请求体、SSE 事件，还是 `ChatResponse`？
- 错误应该停在哪一层：直接抛异常、转成错误事件，还是延后到回调结束再抛？
- 这个改动是否需要同步更新示例和测试，否则调用方式会漂移？

## 当前仓库的推荐流向

### 同步聊天

`Config` / 调用方输入 → `AIClient` 选择 Provider → `DeepSeekProvider` 组装请求 → `HttpClient` 发请求 → Provider 解析响应 → `ChatResponse`

### 流式聊天

`Config` / 调用方输入 → `AIClient::streamChat` → `DeepSeekProvider` 建立流式请求 → `HttpClient::postStream` 接收分块 → Provider 缓冲完整 SSE 事件 → `SSEParser` 解释事件 → 回调上抛增量

## 需要同步检查的边界

- 改 `Config`：检查 `loadConfigFromFile`、`.env` 装载、示例和联机测试是否还能找到配置。
- 改 `ChatRequest` / `Message`：检查 Provider 请求体序列化和相关单元测试。
- 改 `DeepSeekProvider` 请求体：检查同步与流式两条路径是否一致。
- 改 `HttpClient`：检查普通 POST、流式 POST 和回调异常传播。
- 改 `SSEParser`：检查 `null` 内容、错误事件、`[DONE]` 和尾块刷新。

## 常见跨层失配

- 上层新增字段，但 Provider 请求体没带出去。
- Provider 解析已经容忍 `null`，上层却又把它当成错误。
- 网络层吞掉异常，导致调用方看不到真实失败原因。
- 示例仍按旧接口调用，测试却覆盖的是新接口。
