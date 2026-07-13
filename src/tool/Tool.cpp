#include "tool/Tool.h"

#include <utility>

namespace aiSDK {

ToolResult ToolResult::successResult(nlohmann::json result) {
    // 成功结果保留任意 JSON 值，便于工具返回对象、数组或标量。
    // error_message 固定为空，保证调用方只需先判断 success，
    // 不会同时面对“成功数据”和“失败文本”两个互相冲突的信号。
    return ToolResult{true, std::move(result), ""};
}

ToolResult ToolResult::errorResult(std::string message) {
    // 失败结果不混入伪造数据，调用方只需检查 success 和 error_message。
    // data 使用空对象而不是 null，便于日志和后续 JSON 导出保持稳定结构；
    // 具体错误文本按值移动，避免扩展工具返回临时字符串时产生悬空引用。
    return ToolResult{false, nlohmann::json::object(), std::move(message)};
}

}  // namespace aiSDK
