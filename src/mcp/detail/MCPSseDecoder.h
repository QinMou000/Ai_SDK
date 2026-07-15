#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aiSDK {
namespace detail {

// MCPSseEvent 是传输层消费的一次完整 SSE 事件。
// id 只表示当前事件显式携带的游标；空字符串用于清除旧游标。
// retry 已按本地上限裁剪，调用方无需再次处理整数溢出。
// data 保留多行字段之间的换行，has_data 区分缺失与显式空 data。
struct MCPSseEvent {
    std::optional<std::string> id;
    std::optional<std::chrono::milliseconds> retry;
    std::string data;
    bool has_data = false;
};

// MCPSseDecoder 对网络字节执行有状态增量解码。
// feed 可以接收任意分块，事件只在空行分隔符完整到达后返回。
// 解码器只解释 SSE framing，不解析 data 中的 JSON-RPC 消息。
// 所有限制按原始 UTF-8 字节计数，避免长期流造成无界缓存。
class MCPSseDecoder {
   public:
    // 三个上限分别约束单事件、事件 ID 和 Server 建议的重连等待。
    // 单事件和 ID 上限必须大于零；retry 上限允许为零但不能为负。
    MCPSseDecoder(std::size_t max_event_bytes, std::size_t max_event_id_bytes, std::chrono::milliseconds max_retry);

    // feed 消费本次网络分块，并按输入顺序返回已经完成的事件。
    // 注释和未知字段参与事件大小限制，但不会产生业务事件。
    // 输入包含 NUL、非法 retry 或任一硬上限被突破时抛出中文 runtime_error。
    // 非法 retry 属于协议错误；调用方不得把它降级为可忽略的未知字段。
    std::vector<MCPSseEvent> feed(std::string_view chunk);

    // finish 表示当前流已经结束；没有空行终止的尾部必须丢弃。
    // 方法同时清空状态，使同一实例可以安全解码下一条独立流。
    std::vector<MCPSseEvent> finish();

    // reset 主动丢弃当前残片和事件字段，不生成任何伪造事件。
    void reset() noexcept;

   private:
    // accountEventByte 在缓存字节前执行上限检查。
    void accountEventByte();
    // completeLine 处理一个已移除换行符的完整字段行。
    void completeLine(std::vector<MCPSseEvent>& events);
    // processLine 按 SSE 的首个冒号规则解释字段和值。
    void processLine();
    // dispatchEvent 只分派包含 data、id 或 retry 的事件。
    void dispatchEvent(std::vector<MCPSseEvent>& events);
    // parseRetry 接受可表示的纯十进制非负整数，并按构造上限安全裁剪。
    // 空值、符号、单位或整数溢出会直接抛出协议解析异常。
    std::chrono::milliseconds parseRetry(std::string_view value) const;
    // clearCurrentEvent 保留跨块解析器本身，只清理当前事件字段。
    void clearCurrentEvent() noexcept;

    std::size_t max_event_bytes_;
    std::size_t max_event_id_bytes_;
    std::chrono::milliseconds max_retry_;

    // line_buffer_ 不包含行结束符；pending_cr_ 用于识别跨块 CRLF。
    std::string line_buffer_;
    bool pending_cr_ = false;
    std::size_t event_bytes_ = 0;

    // 以下字段在空行到达前持续聚合，重复 id/retry 使用最后一个值。
    // 任一非法 retry 会终止整条流，不能退回到同一事件内的旧合法值。
    std::optional<std::string> current_id_;
    std::optional<std::chrono::milliseconds> current_retry_;
    std::string current_data_;
    bool current_has_data_ = false;
};

}  // namespace detail
}  // namespace aiSDK
