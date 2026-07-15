#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mcp/MCPTypes.h"

namespace aiSDK {

// MCPToolCatalogAccess 是实现目标内部唯一的目录签发访问器。
// 公共头只前置声明该类型，应用无法在不依赖私有源码的情况下伪造令牌。
// 访问器没有独立状态，也不延长 Client 寿命，只读取 Catalog 已持有的共享令牌。
// create 的调用点负责在 Client 锁内取得一致的 Server、revision 和 token 快照。
// 读取函数全部 noexcept，确保目录拒绝路径不会被辅助访问本身打断。
// 该类型只解决封装访问，不承担当前代次判断或远端工具名称查询。
class MCPToolCatalogAccess {
   public:
    // create 是唯一签发入口，完整移动目录数据并共享 Client 私有令牌。
    // 调用方仍需保证 revision 来自当前 Client 的单调代次。
    static MCPToolCatalog create(std::string server_id, std::uint64_t revision, std::vector<MCPTool> tools,
                                 std::shared_ptr<const void> issuer_token) {
        return MCPToolCatalog(std::move(server_id), revision, std::move(tools), std::move(issuer_token));
    }

    static const std::shared_ptr<const void>& issuerToken(const MCPToolCatalog& catalog) noexcept {
        // 返回 const 引用避免改变令牌身份，也避免仅为校验增加引用计数。
        return catalog.issuer_token_;
    }

    static const std::string& serverId(const MCPToolCatalog& catalog) noexcept {
        // Server 标识用于拒绝跨 Client 或跨 Server 的目录误用。
        return catalog.server_id_;
    }

    static std::uint64_t revision(const MCPToolCatalog& catalog) noexcept {
        // 代次值与 Client 当前 revision 精确相等时目录才可用于提交。
        return catalog.revision_;
    }
};

}  // namespace aiSDK
