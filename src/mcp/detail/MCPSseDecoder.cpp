#include "mcp/detail/MCPSseDecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace aiSDK {
namespace detail {

// 增量解码遵守以下状态不变量：
// 1. line_buffer_ 始终只保存尚未遇到行结束符的字段内容。
// 2. pending_cr_ 为真时，前一个输入字节是尚待判定的 CR。
// 3. CRLF 被视为一个行结束，不会额外生成空字段行。
// 4. 非空字段行结束后立即解释，事件仅在后续空行时提交。
// 5. finish 不是协议分隔符，不能提交缺少空行的尾部事件。
// 6. reset 会同时清理行级状态和事件级状态，禁止跨流继承。
// 7. event_bytes_ 统计当前事件所有非空原始字段行。
// 8. 注释与未知字段虽然被忽略，仍必须受事件大小约束。
// 9. 事件分隔空行不计入前后任一事件的字节预算。
// 10. ID 上限按 UTF-8 字节而不是字符数量计算。
// 11. 空 ID 是有效值，用于让上层清除当前恢复游标。
// 12. retry 只接受 ASCII 十进制数字，不接受符号或空白。
// 13. 合法 retry 超过本地策略时裁剪，不改变十进制字段的合法性。
// 14. 非法 retry 立即终止当前流，不能保留同一事件中的旧合法值。
// 15. 多个 data 字段严格按到达顺序用单个换行连接。
// 16. has_data 区分未出现 data 与出现值为空的 data。
// 17. data 原文不在本层解析，JSON-RPC 责任留给协议层。
// 18. 当前事件中的重复 id 和 retry 采用最后一个值，前提是每个值都合法。
// 19. 注释、未知字段和普通空行不会独立产生业务事件。
// 20. id 或 retry 控制字段可产生无 data 的控制事件。
// 21. 任意 NUL 都在字段解释前拒绝，避免字符串边界歧义。
// 22. 任一解析异常都会清空缓存，故障字节不会继续驻留。
// 23. 已完成事件按输入顺序返回，不执行重排或去重。
// 24. 解码过程不调用用户回调，也不持有网络或 Client 锁。
// 25. 每个输入字节只处理常数次，整体时间复杂度为 O(n)。
// 26. 缓冲规模由事件上限约束，长期连接不会累计完整正文。
// 27. 返回事件拥有自己的字符串，不引用 feed 的临时输入内存。
// 28. 空分块保持全部状态不变，也不会产生空事件。
// 29. 上限判断在追加字节前完成，越界字节不会进入缓存。
// 30. 解码器不保存跨事件游标，游标归属由每条 HTTP 流维护。

// 构造阶段只验证静态资源预算，不读取网络或启动后台任务。
// retry 使用 chrono 类型避免调用方混淆秒与毫秒。
MCPSseDecoder::MCPSseDecoder(std::size_t max_event_bytes, std::size_t max_event_id_bytes,
                             std::chrono::milliseconds max_retry)
    : max_event_bytes_(max_event_bytes), max_event_id_bytes_(max_event_id_bytes), max_retry_(max_retry) {
    if(max_event_bytes_ == 0U) {
        throw std::invalid_argument("SSE 单事件上限必须大于零");
    }
    if(max_event_id_bytes_ == 0U) {
        throw std::invalid_argument("SSE 事件 ID 上限必须大于零");
    }
    if(max_retry_.count() < 0) {
        throw std::invalid_argument("SSE retry 上限不能为负数");
    }
}

// 每个非空字段行的原始字节都计入事件预算。
// 分隔事件的空行不属于任一事件，因此不会消耗该预算。
void MCPSseDecoder::accountEventByte() {
    if(event_bytes_ >= max_event_bytes_) {
        throw std::runtime_error("SSE 单个事件超过允许上限");
    }
    ++event_bytes_;
}

// retry 必须完整由 ASCII 数字组成；空值、符号、空白、单位和小数都属于协议违规。
// 先按 duration 表示范围解析，再应用本地策略裁剪，避免把整数溢出伪装成合法大值。
std::chrono::milliseconds MCPSseDecoder::parseRetry(std::string_view value) const {
    if(value.empty()) {
        throw std::runtime_error("SSE retry 必须是十进制非负整数");
    }
    for(const char character : value) {
        if(character < '0' || character > '9') {
            throw std::runtime_error("SSE retry 必须是十进制非负整数");
        }
    }

    using Rep = std::chrono::milliseconds::rep;
    constexpr Rep maximum = std::numeric_limits<Rep>::max();
    constexpr Rep maximum_quotient = maximum / 10;
    constexpr Rep maximum_remainder = maximum % 10;
    Rep parsed = 0;
    for(const char character : value) {
        const Rep digit = static_cast<Rep>(character - '0');
        // 商和余数比较在乘加前识别精确溢出边界，不执行未定义的有符号运算。
        if(parsed > maximum_quotient || (parsed == maximum_quotient && digit > maximum_remainder)) {
            throw std::runtime_error("SSE retry 超出整数表示范围");
        }
        parsed = static_cast<Rep>(parsed * 10 + digit);
    }
    return std::chrono::milliseconds{std::min(parsed, max_retry_.count())};
}

// 字段在首个冒号处分割；值开头至多移除一个可选空格。
// 注释、event 以及所有未知字段均被忽略，但其原始字节已经计入预算。
void MCPSseDecoder::processLine() {
    if(line_buffer_.empty() || line_buffer_.front() == ':') {
        return;
    }

    const std::string_view line{line_buffer_};
    const std::size_t separator = line.find(':');
    const std::string_view field = line.substr(0, separator);
    std::string_view value;
    if(separator != std::string_view::npos) {
        value = line.substr(separator + 1U);
        if(!value.empty() && value.front() == ' ') {
            value.remove_prefix(1U);
        }
    }

    if(field == "data") {
        // 每个 data 字段之间保留一个换行；显式空字段仍会设置 has_data。
        // 不在每行尾部追加换行，可以天然避免事件提交时再删除末尾字符。
        if(current_has_data_) {
            current_data_.push_back('\n');
        }
        // 无冒号的 data 会得到空 string_view，避免把空视图的指针交给 append。
        if(!value.empty()) {
            current_data_.append(value.data(), value.size());
        }
        current_has_data_ = true;
        return;
    }

    if(field == "id") {
        // NUL 已在 feed 入口统一拒绝，这里只需要执行字节长度限制。
        // 赋值而非追加符合重复 id 使用最后值的恢复游标语义。
        if(value.size() > max_event_id_bytes_) {
            throw std::runtime_error("SSE 事件 ID 超过允许上限");
        }
        current_id_ = std::string{value};
        return;
    }

    if(field == "retry") {
        // 非法值由 parseRetry 抛出；feed 的统一异常路径会清空全部攻击载荷状态。
        // 合法值仍按本地策略裁剪，保存的 duration 永远不超过配置上限。
        current_retry_ = parseRetry(value);
    }
}

// 空行是唯一事件提交点；EOF 或 finish 不会调用本函数伪造尾部事件。
// id/retry 控制事件即使没有 data 也要返回，供每条流独立维护恢复状态。
void MCPSseDecoder::dispatchEvent(std::vector<MCPSseEvent>& events) {
    if(current_has_data_ || current_id_.has_value() || current_retry_.has_value()) {
        // 移动较大的 data 与 id，避免长期流中重复复制协议正文。
        MCPSseEvent event;
        event.id = std::move(current_id_);
        event.retry = current_retry_;
        event.data = std::move(current_data_);
        event.has_data = current_has_data_;
        events.push_back(std::move(event));
    }
    clearCurrentEvent();
}

// completeLine 在确认完整 LF、CRLF 或 CR 行结束后调用。
// 非空行只聚合字段，空行才结束当前事件并重置事件字节计数。
void MCPSseDecoder::completeLine(std::vector<MCPSseEvent>& events) {
    if(line_buffer_.empty()) {
        // 连续空行只会重复清理空状态，不会产生空业务事件。
        dispatchEvent(events);
        event_bytes_ = 0U;
        return;
    }

    processLine();
    // 字段值已经复制或追加到事件状态，行缓存可以立即复用。
    line_buffer_.clear();
}

// feed 按字节处理换行，因此 CRLF 即使落在两个网络分块中也不会产生空行。
// 任一错误都会清理当前状态；调用方仍应终止故障流，不应继续拼接旧数据。
std::vector<MCPSseEvent> MCPSseDecoder::feed(std::string_view chunk) {
    std::vector<MCPSseEvent> events;
    try {
        for(const char character : chunk) {
            // NUL 无论出现在字段名、字段值还是注释中都属于非法输入。
            if(character == '\0') {
                throw std::runtime_error("SSE 数据包含非法 NUL 字节");
            }

            // 上一块以 CR 结尾时，本字节决定它是 CRLF 还是独立 CR。
            if(pending_cr_) {
                pending_cr_ = false;
                if(character == '\n') {
                    // CR 已经按原始字节计数，此处只补计 LF 后完成同一行。
                    if(!line_buffer_.empty()) {
                        accountEventByte();
                    }
                    completeLine(events);
                    continue;
                }
                // 非 LF 表示前一个 CR 独立结束一行，当前字节需重新解释。
                completeLine(events);
            }

            if(character == '\r') {
                // 只有非空字段行的结束符计入单事件字节预算。
                // 延迟完成可正确处理 CR 与 LF 恰好分属两个网络分块的情况。
                if(!line_buffer_.empty()) {
                    accountEventByte();
                }
                pending_cr_ = true;
                continue;
            }

            if(character == '\n') {
                // 单独 LF 立即完成当前行；空行会触发唯一的事件提交点。
                if(!line_buffer_.empty()) {
                    accountEventByte();
                }
                completeLine(events);
                continue;
            }

            accountEventByte();
            // 普通字节只进入当前行，字段语义要等完整行结束后再解释。
            line_buffer_.push_back(character);
        }
    } catch(...) {
        // 清理可能含攻击载荷的缓存，避免异常后继续占用受限内存。
        reset();
        throw;
    }
    return events;
}

// 流结束不等价于 SSE 空行，残缺字段和未提交事件必须全部丢弃。
std::vector<MCPSseEvent> MCPSseDecoder::finish() {
    // 即使当前行刚好以字符结束，只要没有空行就仍属于未提交事件。
    reset();
    return {};
}

// reset 同时清除跨块 CR 状态，确保下一条流不会继承旧分隔符。
void MCPSseDecoder::reset() noexcept {
    // clear 保留容器容量，重复重连时不会为常见短事件反复申请内存。
    line_buffer_.clear();
    pending_cr_ = false;
    event_bytes_ = 0U;
    clearCurrentEvent();
}

// 事件级清理不会改动静态上限，也不会分配新缓冲。
void MCPSseDecoder::clearCurrentEvent() noexcept {
    // optional::reset 能保留“未出现字段”和“出现空值”之间的区别。
    current_id_.reset();
    current_retry_.reset();
    current_data_.clear();
    current_has_data_ = false;
}

}  // namespace detail
}  // namespace aiSDK
