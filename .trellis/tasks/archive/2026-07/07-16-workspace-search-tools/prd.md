# 实现工作区文件与文本搜索工具

## 目标

在现有受限 UTF-8 工作区文件工具中新增 `find_files` 与 `search_text`，让 `SimpleAgent` 能在调用方显式授权的目录内发现文件并检索文本；不引入 Office 文档解析或外部依赖。

## 已确认的需求

- 工具注册继续收敛在 `registerWorkspaceFileTools(...)`，两项工具均为只读 `Low` 风险工具。
- `find_files` 从可选相对目录 `path` 递归搜索普通文件，按文件名中包含的可选 `query` 过滤，并按稳定顺序返回相对路径。
- `search_text` 从可选相对目录 `path` 递归搜索 UTF-8 小文本文件，使用必填非空 `query` 进行字面量匹配，返回相对路径、从 1 开始的行列号和受限行文本。
- 两项工具复用既有工作区边界、敏感路径、符号链接、UTF-8 与单文件容量规则；不得暴露或访问工作区外文件。
- 搜索结果达到独立上限时返回截断标记；此上限不改变已有单层目录列举上限的语义。
- 必须补充本地自动化测试，并更新受影响的 `SimpleAgent` 工具可见集合断言。

## 验收标准

- [x] 合法目录内的嵌套普通文件可被 `find_files` 发现，结果使用相对路径并稳定排序。
- [x] `find_files` 拒绝越界/敏感路径，不跟随符号链接，达到结果上限时明确标记截断。
- [x] `search_text` 能返回正确的 1 起始行号、列号和文本预览；空查询被拒绝。
- [x] `search_text` 跳过敏感、链接、超限或非 UTF-8 文件，不会因单个无关文件破坏整次搜索。
- [x] 既有工作区文件工具行为和 `SimpleAgent` 的低风险过滤保持正确。
- [x] 使用仓库 Windows Debug 预设完成格式、构建和相关测试的本地验证。

## 技术方案

在 `WorkspaceFileTools.cpp` 内增加受限递归遍历辅助逻辑，复用现有规范化路径、根目录边界和敏感路径判断。首版仅支持字面量文件名/文本查询，不支持 glob、正则、内容索引、二进制文件或 `docx`/`pptx` 解析。结果数量由 `WorkspaceFileToolOptions` 的新增搜索上限控制。

## 决策记录

**背景**：Agent 需要基础的工作区发现和文本定位能力，但此前的 Office 文档支持会引入格式解析、依赖与安全边界。

**决策**：保留 UTF-8 纯文本范围，在既有文件工具模块中增补两个只读搜索工具。

**影响**：Markdown 可直接搜索；Office 文档及复杂查询语义留待后续真实需求驱动时独立设计。

## 范围外

- `docx`、`pptx`、PDF、表格和其他二进制格式解析。
- glob、正则、大小写模糊匹配、全文索引或搜索缓存。
- Shell、网络、Git 命令或工作区写入能力。

## 涉及位置

- `include/agent/WorkspaceFileTools.h`
- `src/agent/WorkspaceFileTools.cpp`
- `tests/agent/workspace_file_tools_test.cpp`
- `tests/agent/simple_agent_test.cpp`
- `README.md`（如工具能力清单需要同步）
- `.trellis/spec/backend/agent-contracts.md`（完成后评估是否沉淀新的工具契约）
