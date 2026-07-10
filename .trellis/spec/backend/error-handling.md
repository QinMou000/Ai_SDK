# 错误处理规范

## 适用范围

本文件覆盖当前 SDK 中最关键的错误边界：

- 配置装载失败
- Provider 选择失败
- HTTP 传输失败
- 非 2xx 服务端响应
- SSE 数据块解析失败
- 流式回调内部抛错

## 现有错误模型

当前代码以 C++ 异常为主，没有自定义统一错误类。调用者通过异常类型和错误消息区分失败来源。

### 常见抛出点

- `src/core/Config.cpp`
  - 打不开配置文件时抛 `std::runtime_error`
  - 非法 JSON 或解析失败时沿用 `nlohmann::json` 异常
- `src/core/Message.cpp`
  - 未知角色字符串抛 `std::invalid_argument`
- `src/AIClient.cpp`
  - Provider 名为空或不支持时抛 `std::invalid_argument`
  - Provider 尚未初始化时抛 `std::logic_error`
- `src/http/HttpClient.cpp`
  - 传输层失败时抛 `std::runtime_error`
  - 流式回调内部异常会被捕获并在请求结束后重新抛出
- `src/provider/DeepSeekProvider.cpp`
  - 缺失 API Key、HTTP 非成功状态、响应体缺字段或格式错误时抛 `std::runtime_error`
- `src/http/SSEParser.cpp`
  - SSE `data:` 不是合法 JSON，或上游返回错误对象时转成错误事件，而不是直接抛异常

## 当前约定

### 1. 初始化参数错误用 `std::invalid_argument`

- 适用于调用者传入的 provider 名为空、角色非法、显式参数不满足前置条件的场景。
- 参考：`AIClient::setProvider` 与 `messageRoleFromString`。

### 2. 生命周期错误用 `std::logic_error`

- 当对象状态不允许当前调用时使用，例如 Provider 尚未可用却调用聊天接口。
- 参考：`AIClient::chat`、`AIClient::streamChat`。

### 3. 运行期外部失败用 `std::runtime_error`

- 文件打不开、HTTP 失败、远端返回错误、缺失必需鉴权信息，都归入运行期失败。
- 错误消息必须包含足够的上下文，让调用方能区分是本地配置问题、网络问题还是远端拒绝。

### 4. 流式协议错误优先留在事件层

- `SSEParser` 当前返回 `SSEEvent`，其中错误通过 `SSEEventType::Error` 表达。
- `DeepSeekProvider::streamChat` 负责把错误事件转交回调；不要在解析器里过早混入网络重试或 UI 文案。

## 验证与错误矩阵

| 触发条件 | 当前行为 | 参考文件 |
| --- | --- | --- |
| 配置文件不存在 | 抛 `std::runtime_error`，消息含 `failed to open config file` | `src/core/Config.cpp` |
| 消息角色字符串非法 | 抛 `std::invalid_argument` | `src/core/Message.cpp` |
| Provider 名为空 | 抛 `std::invalid_argument` | `src/AIClient.cpp` |
| Provider 未注册或不支持 | 抛 `std::invalid_argument` | `src/AIClient.cpp` |
| Provider 尚未初始化却调用 | 抛 `std::logic_error` | `src/AIClient.cpp` |
| HTTP 传输失败 | 抛 `std::runtime_error` | `src/http/HttpClient.cpp` |
| 非 2xx 且返回可解析错误体 | 抛 `std::runtime_error`，优先拼接远端错误信息 | `src/provider/DeepSeekProvider.cpp` |
| SSE `data:` 非法 JSON | 生成 `Error` 事件 | `src/http/SSEParser.cpp` |
| 流式回调内部抛错 | `HttpClient` 缓存异常并在请求返回后重新抛出 | `src/http/HttpClient.cpp` |

## 新代码应遵循的模式

- 在边界处尽早判空、判非法值，避免把无效状态带入网络层。
- 错误消息优先写“失败动作 + 原因”，例如“创建 DeepSeek 请求失败：缺少 API Key”。
- 不要吞掉底层异常后只返回模糊的“unknown error”。
- 流式链路里不要把每个解析异常都直接 `throw` 到传输层，先判断是否更适合表达为错误事件。

## 常见错误

- 把外部服务错误都映射成 `invalid_argument`，导致调用者误判为本地参数问题。
- 在底层捕获异常后不补上下文，最终只剩 JSON 库原始报错。
- 让解析器、HTTP 客户端和 Provider 同时决定错误文案，造成重复和冲突。
