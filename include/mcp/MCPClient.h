#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "mcp/IMCPTransport.h"
#include "mcp/MCPServerConfig.h"
#include "mcp/MCPTypes.h"

namespace aiSDK {

// MCPClient 是单个 MCP Server 配置与逻辑会话的同步门面。
// 一个实例只允许一个用户公开操作在途，后台传输控制不占该名额。
class MCPClient {
   public:
    // 回调参数刻意不包含工具正文，避免 Listener 线程把不受信任数据扩散到上层。
    using CatalogChangedCallback = std::function<void(const std::string& server_id, std::uint64_t revision)>;

    // 构造函数只校验配置并固定依赖；传入 transport 用于确定性本地测试。
    explicit MCPClient(MCPServerConfig config, std::shared_ptr<IMCPTransport> transport = nullptr);
    ~MCPClient() noexcept;

    MCPClient(const MCPClient&) = delete;
    MCPClient& operator=(const MCPClient&) = delete;
    MCPClient(MCPClient&&) = delete;
    MCPClient& operator=(MCPClient&&) = delete;

    // connect 完成 initialize、能力协商、initialized 通知和首次 Listener 分类。
    void connect();
    // ping 只验证连接与协议往返，不依赖工具目录代次。
    void ping();
    // listTools 完成有界分页并签发不可伪造的目录快照。
    MCPToolCatalog listTools();
    // callTool 只接受当前 Client 签发且未失效的 Catalog。
    // arguments 顶层必须是对象；请求一旦提交后丢失终局会提升为 OutcomeUnknown。
    MCPToolCallResult callTool(const MCPToolCatalog& catalog, const std::string& remote_name,
                               const nlohmann::json& arguments);
    // close 可与公开操作并发调用，最终进入 Closed 且不抛异常。
    void close() noexcept;

    // 以下读取入口线程安全且不占前台公开操作槽。
    // 状态值是调用瞬间的快照；读取后状态仍可能被 Listener 或 close 推进。
    MCPClientState state() const noexcept;
    MCPListenerState listenerState() const noexcept;
    std::optional<MCPErrorCode> lastFailureCode() const noexcept;
    std::optional<MCPErrorCode> lastListenerFailureCode() const noexcept;
    std::uint64_t catalogRevision() const noexcept;
    bool isToolCatalogStale() const noexcept;
    const std::string& serverId() const noexcept;

    // 目录回调只发送 Server 标识与新代次；回调异常由 Client 隔离。
    // 回调在 Client 内部锁外执行，调用方不得假设与公开操作处于同一线程。
    void setCatalogChangedCallback(CatalogChangedCallback callback);

   private:
    friend class MCPToolAdapter;

    // ownsCatalog 供受信 Adapter 在生成 Binding 前校验签发身份与当前代次。
    // 该入口保持私有，应用仍只能通过 callTool 使用 Catalog。
    // 校验只读且不抛异常，失败目录不会产生任何传输提交。
    bool ownsCatalog(const MCPToolCatalog& catalog) const noexcept;

    class Impl;
    // PImpl 隐藏线程、队列、协议和具体 Transport 状态，公开头不泄露三方类型。
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiSDK
