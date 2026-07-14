#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/ChatRequest.h"
#include "core/ChatResponse.h"
#include "core/Config.h"
#include "http/SSEParser.h"
#include "provider/IModelProvider.h"
#include "tool/ToolExecutor.h"
#include "tool/ToolRegistry.h"
#include "trace/TraceRecorder.h"

namespace aiSDK {

// AIClient 是 SDK 的统一入口。
// 当前阶段先稳定配置管理、Provider 选择和基础请求委托链路。
class AIClient {
   public:
    // config 允许按值传入，便于调用方使用临时配置快速构造客户端。
    explicit AIClient(Config config = {});

    // 这些访问器用于读取当前运行时状态，而不暴露可变内部对象。
    const Config& config() const;
    const std::string& activeProviderName() const;

    // startTrace 根据 Config::enable_trace 创建显式链路会话。
    // 关闭开关时返回禁用句柄，不生成 ID，也不会保存后续步骤。
    TraceSession startTrace(TraceOptions options = {}) const;

    // chat / streamChat 将请求委托给当前激活的 Provider 执行。
    // 后续接入真实网络层时，上层调用方式无需变化。
    ChatResponse chat(const ChatRequest& request) const;
    // Trace 重载把本次模型请求作为新根步骤写入调用方提供的会话。
    // 多次公开调用可复用同一会话，但 SDK 不会推断前后调用的父子关系。
    ChatResponse chat(const ChatRequest& request, TraceSession& trace_session) const;
    void streamChat(const ChatRequest& request, StreamCallback callback) const;
    // 流式重载保持原回调与异常语义，只额外记录一次流级汇总。
    void streamChat(const ChatRequest& request, StreamCallback callback, TraceSession& trace_session) const;

    // tools 暴露本地工具注册表，供后续 ToolCall 链路复用。
    ToolRegistry& tools();
    const ToolRegistry& tools() const;

    // executeToolCalls 只执行调用方显式传入的一批工具调用。
    // calls 必须来自调用方选定的模型响应；返回值与输入顺序一一对应。
    // 未知工具或处理函数异常会收敛到 ToolExecutionResult::result，
    // 不会修改消息历史，也不会自动发起下一轮模型请求。
    std::vector<ToolExecutionResult> executeToolCalls(const std::vector<ToolCall>& calls);
    // Trace 重载记录一个批次步骤及其工具子步骤，仍按输入顺序串行执行。
    std::vector<ToolExecutionResult> executeToolCalls(const std::vector<ToolCall>& calls, TraceSession& trace_session);

    // setProvider 切换当前激活的 Provider，并立即绑定对应实现。
    void setProvider(const std::string& provider_name);
    // 实例重载允许调用方接入自定义 Provider，也为本地确定性测试提供注入点。
    // Provider 名称取自 info()，空指针或空名称会在替换当前实例前被拒绝。
    void setProvider(std::shared_ptr<IModelProvider> provider);

   private:
    Config config_;
    std::string active_provider_name_;
    ToolRegistry tool_registry_;
    // active_provider_ 保存当前生效的 Provider 实例，避免每次请求重复构造。
    std::shared_ptr<IModelProvider> active_provider_;
};

}  // namespace aiSDK
