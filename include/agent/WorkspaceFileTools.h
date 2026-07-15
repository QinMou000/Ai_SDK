#pragma once

#include <cstddef>
#include <filesystem>

#include "tool/ToolRegistry.h"

namespace aiSDK {

// WorkspaceFileToolOptions 描述一组文件工具唯一允许访问的工作区。
// 选项独立于 SimpleAgent，便于未来被审批层、MCP 适配器或其他 Agent 复用。
struct WorkspaceFileToolOptions {
    // root 是调用方为当前 Agent 实例授予的唯一文件系统范围。
    std::filesystem::path root;
    std::size_t max_file_bytes = 64U * 1024U;
    std::size_t max_directory_entries = 256U;
};

// registerWorkspaceFileTools 在注册表中加入受限工作区文件工具。
// 注册过程会规范化根目录并拒绝重复名称；所有处理函数仅捕获该不可变配置值。
// 函数不拥有注册表，调用方应保证注册阶段与后续执行阶段不存在并发修改。
void registerWorkspaceFileTools(ToolRegistry& registry, const WorkspaceFileToolOptions& options);

}  // namespace aiSDK
