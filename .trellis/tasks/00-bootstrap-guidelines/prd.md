# 初始化任务：补齐项目开发规格

本任务用于把 `trellis init` 生成的通用模板，收敛成当前仓库真实可执行的开发规格。目标不是写理想化说明，而是把后续 AI 会话真正需要遵守的边界、契约和验证方式沉淀到 `.trellis/spec/`。

## 目标

- 让未来的 `trellis-implement`、`trellis-check` 或 Codex Inline 会话优先读取真实项目规则，而不是前后端通用模板。
- 让规格内容能够直接映射到当前 C++ SDK 的源代码、测试和构建预设。

## 当前范围

- 规格目录：`.trellis/spec/backend/`、`.trellis/spec/guides/`
- 任务目录：`.trellis/tasks/00-bootstrap-guidelines/`
- 真实代码来源：
  - `CMakeLists.txt`
  - `CMakePresets.json`
  - `include/`
  - `src/`
  - `tests/`
  - `examples/`

## 明确不在范围内

- 不修改产品源码行为。
- 不补做“理想中的前端层”；当前仓库没有前端源代码与前端构建入口。
- 不归档任务，不执行提交。

## 架构结论

- 当前仓库是单仓库 C++17 AI SDK。
- 对外入口是 `AIClient`，供应商抽象是 `IModelProvider`，唯一落地实现是 DeepSeek。
- HTTP 与流式解析由 `HttpClient`、`SSEParser` 负责。
- 测试基于 GTest，构建与测试通过 `CMakePresets.json` 预设驱动。
- `.trellis/spec/frontend/` 是初始化模板遗留，与当前代码库不匹配，应移除。

## 需要交付的规格内容

- 后端索引、目录结构、错误处理、日志、质量规范。
- 一份基于真实代码的跨模块契约文档，覆盖 `AIClient`、Provider、HTTP/SSE 和配置边界。
- 通用思考指南，聚焦跨层数据流与代码复用，而不是通用前后端建议。

## 完成状态

- [x] 填充后端规格
- [x] 确认当前仓库无前端层并移除不适用模板
- [x] 补充基于真实源码的契约、错误矩阵和验证要求

## 验收标准

- `.trellis/spec/` 中不再保留模板占位语句。
- `backend/index.md` 与实际规格文件集合一致。
- 规格中的关键结论都能追溯到源码、测试或构建文件。
- `python ./.trellis/scripts/get_context.py --mode packages` 能正确识别当前规格结构。
