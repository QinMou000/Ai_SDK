#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "mcp/detail/MCPSseDecoder.h"

namespace {

using aiSDK::detail::MCPSseDecoder;
using aiSDK::detail::MCPSseEvent;

// 本文件的测试矩阵遵守以下验证边界：
// 1. 所有输入都直接送入真实增量解码器，不使用解析替身。
// 2. 测试不启动网络服务，分块由确定性 string_view 精确控制。
// 3. LF 与 CRLF 均覆盖，特别验证 CR 和 LF 位于不同分块。
// 4. 每个字节切分点都独立构造解码器，避免状态互相污染。
// 5. 单字节喂入补充覆盖连续多分块而不只是一次切分。
// 6. 多行 data 断言最终换行位置，而非只检查事件数量。
// 7. has_data 单独断言，确保显式空值不会退化为字段缺失。
// 8. 空 ID 仍由 optional 承载，上层可以据此清除游标。
// 9. 注释和未知字段只能影响资源预算，不能产生业务事件。
// 10. 非法 retry 必须中止当前流，不能保留同一事件中的合法 data。
// 11. 可表示的大 retry 允许裁剪，整数表示溢出必须作为协议错误拒绝。
// 12. 单事件与 ID 上限使用不同实例，便于定位失败责任。
// 13. NUL 测试构造真实内嵌零字节，不依赖 C 字符串截断。
// 14. finish 明确验证 EOF 不会充当事件终止空行。
// 15. reset 同时验证行状态、CR 状态和事件字段被清除。
// 16. 错误消息至少包含中文稳定分类，不匹配底层动态细节。
// 17. 构造参数测试在任何输入前执行，保证无网络副作用。
// 18. 所有时间值都显式使用毫秒，避免单位推断。
// 19. 测试预算远小于生产值，以较短载荷稳定触达边界。
// 20. 测试不依赖休眠、端口、环境变量或平台专有行为。
// 21. 返回字符串按值断言，确保事件不引用已经销毁的输入分块。
// 22. 控制事件与 data 事件分别断言，避免恢复逻辑语义混淆。
// 23. 重复字段采用最后合法值的行为由组合输入间接覆盖。
// 24. 超限后复用实例，验证攻击载荷已经从内部缓存清理。
// 25. 空分块由切分点零和末尾切分自然覆盖。
// 26. 未知字段与注释同时覆盖 LF 和 CRLF 两种输入形态。
// 27. 每个测试只验证一个主要合同，失败定位保持清晰。
// 28. 边界断言先检查事件数量，再安全访问字段与正文。
// 29. optional 字段访问前始终 ASSERT，避免测试自身未定义行为。
// 30. 这些用例只验证 framing，不对 data 执行 JSON 语义判断。

// appendEvents 保留多次 feed 的事件顺序，便于测试任意网络分块。
void appendEvents(std::vector<MCPSseEvent>& target, std::vector<MCPSseEvent> source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

// makeDecoder 使用很小但足够的确定性预算，测试不会依赖生产默认值。
MCPSseDecoder makeDecoder() {
    return MCPSseDecoder(1024U, 64U, std::chrono::milliseconds{3000});
}

// incrementDecimal 只用于构造恰好超过 duration::rep 最大值的十进制文本。
// 它在字符串上执行进位，测试本身不会先触发 C++ 整数溢出。
std::string incrementDecimal(std::string value) {
    for(std::size_t index = value.size(); index > 0U; --index) {
        char& digit = value[index - 1U];
        if(digit != '9') {
            ++digit;
            return value;
        }
        digit = '0';
    }
    value.insert(value.begin(), '1');
    return value;
}

// expectRuntimeErrorContains 同时锁定异常类型与中文稳定分类。
// 动态长度或原始输入不会进入错误文本，避免测试鼓励敏感信息泄漏。
template <typename Operation>
void expectRuntimeErrorContains(Operation operation, std::string_view expected_text) {
    try {
        operation();
        FAIL() << "预期抛出运行期异常，但操作正常返回";
    } catch(const std::runtime_error& error) {
        EXPECT_NE(std::string{error.what()}.find(expected_text), std::string::npos);
    } catch(...) {
        FAIL() << "抛出的异常类型不是 std::runtime_error";
    }
}

// 正常事件同时验证 CRLF、多行 data、游标和 retry 的完整映射。
// 注释、event 和未知字段不能污染业务数据，也不能额外产生事件。
TEST(MCPSseDecoderTest, ParsesFieldsAndCursorWarmupEvents) {
    MCPSseDecoder decoder = makeDecoder();
    const std::string input =
        ": 保活注释\r\n"
        "event: message\r\n"
        "unknown: ignored\r\n"
        "id: cursor-1\r\n"
        "retry: 2500\r\n"
        "data: first\r\n"
        "data: second\r\n"
        "\r\n"
        "id: warmup\r\n"
        "data:\r\n"
        "\r\n";

    const std::vector<MCPSseEvent> events = decoder.feed(input);
    // 第一项是完整消息，第二项是只更新游标的预热控制事件。
    ASSERT_EQ(events.size(), 2U);

    // 完整消息的四个字段必须同时保留，不能把 retry 混入 data。
    ASSERT_TRUE(events[0].id.has_value());
    EXPECT_EQ(*events[0].id, "cursor-1");
    ASSERT_TRUE(events[0].retry.has_value());
    EXPECT_EQ(*events[0].retry, std::chrono::milliseconds{2500});
    EXPECT_TRUE(events[0].has_data);
    EXPECT_EQ(events[0].data, "first\nsecond");

    // 预热帧需要更新游标，但显式空 data 不能被误判成字段缺失。
    ASSERT_TRUE(events[1].id.has_value());
    EXPECT_EQ(*events[1].id, "warmup");
    EXPECT_FALSE(events[1].retry.has_value());
    EXPECT_TRUE(events[1].has_data);
    EXPECT_TRUE(events[1].data.empty());
}

// 每个可能的单一切分点都重新创建解码器，覆盖字段、LF 和 CRLF 中间断开。
// 同一负载再按单字节喂入，证明状态不会依赖网络库的分块大小。
TEST(MCPSseDecoderTest, ParsesEveryChunkBoundary) {
    const std::string input = "id: cursor\r\nretry: 42\r\ndata: {\"jsonrpc\":\"2.0\"}\r\n\r\n";

    for(std::size_t split = 0U; split <= input.size(); ++split) {
        // SCOPED_TRACE 只报告失败切分位置，不改变解码器输入。
        SCOPED_TRACE(split);
        MCPSseDecoder decoder = makeDecoder();
        std::vector<MCPSseEvent> events;
        appendEvents(events, decoder.feed(std::string_view{input}.substr(0U, split)));
        appendEvents(events, decoder.feed(std::string_view{input}.substr(split)));

        // 每一种切分都必须得到与整块输入完全相同的一条事件。
        ASSERT_EQ(events.size(), 1U);
        ASSERT_TRUE(events[0].id.has_value());
        EXPECT_EQ(*events[0].id, "cursor");
        ASSERT_TRUE(events[0].retry.has_value());
        EXPECT_EQ(*events[0].retry, std::chrono::milliseconds{42});
        EXPECT_EQ(events[0].data, "{\"jsonrpc\":\"2.0\"}");
    }

    MCPSseDecoder byte_decoder = makeDecoder();
    std::vector<MCPSseEvent> byte_events;
    // 循环变量在 feed 返回前保持有效，string_view 不会被解码器保存。
    for(const char character : input) {
        appendEvents(byte_events, byte_decoder.feed(std::string_view{&character, 1U}));
    }
    ASSERT_EQ(byte_events.size(), 1U);
    EXPECT_EQ(byte_events[0].data, "{\"jsonrpc\":\"2.0\"}");
}

// 纯注释、空行和未知字段属于合法保活，不应进入 JSON-RPC 分发。
// 名称相似的未知字段仍被忽略；合法 retry 超出本地策略时继续安全裁剪。
TEST(MCPSseDecoderTest, IgnoresUnknownFieldsAndClampsValidRetry) {
    MCPSseDecoder decoder = makeDecoder();
    const std::string input =
        ": only-comment\n\n"
        "unknown: value\n\n"
        "x-retry: -1\n"
        "retry-after: 12ms\n"
        "retry: 3001\n"
        "data: message\n\n";

    const std::vector<MCPSseEvent> events = decoder.feed(input);
    // 前两个保活块及两个未知字段均不产生事件，只留下合法 retry 与 data。
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(events[0].retry.has_value());
    EXPECT_EQ(*events[0].retry, std::chrono::milliseconds{3000});
    EXPECT_EQ(events[0].data, "message");
}

// retry 只接受没有符号、空白、小数或单位的 ASCII 十进制非负整数。
// 每个非法输入使用独立实例，保证前一个异常的 reset 不会掩盖后续分类。
TEST(MCPSseDecoderTest, RejectsInvalidRetrySyntax) {
    // 空值覆盖字段缺少整数，其他成员分别覆盖负号、正号、单位、小数和两类额外空白。
    // 表驱动输入只保存字段值，统一拼接保证字段名与换行边界完全一致。
    const std::vector<std::string> invalid_values{"", "-1", "+1", "12ms", "1.5", " 1", "\t1"};
    for(const std::string& value : invalid_values) {
        SCOPED_TRACE(value);
        MCPSseDecoder decoder = makeDecoder();
        // 冒号后的第一个空格属于 SSE 语法可选分隔，不属于待解析整数。
        // value 自带的第二个空格或制表符则必须保留，并由严格解析拒绝。
        const std::string input = std::string{"retry: "} + value + "\n\n";
        expectRuntimeErrorContains([&] { decoder.feed(input); }, "retry 必须是十进制非负整数");
    }

    // 没有冒号的 retry 字段等价于空值，同样不能被当成未知字段静默忽略。
    MCPSseDecoder missing_value = makeDecoder();
    expectRuntimeErrorContains([&] { missing_value.feed("retry\n\n"); }, "retry 必须是十进制非负整数");

    // 同一事件中先合法后非法仍必须失败，不能退回并发布旧 retry 或 data。
    MCPSseDecoder duplicate_retry = makeDecoder();
    expectRuntimeErrorContains([&] { duplicate_retry.feed("retry: 10\nretry: -1\ndata: forbidden\n\n"); },
                               "retry 必须是十进制非负整数");
}

// duration::rep 的最大非负整数仍是合法值，只按本地三秒策略裁剪。
// 比该边界大一的纯数字虽然语法正确，但无法表示，必须作为协议错误拒绝。
TEST(MCPSseDecoderTest, RejectsRetryIntegerOverflowButAcceptsRepresentationBoundary) {
    using RetryRep = std::chrono::milliseconds::rep;
    // 从当前标准库实际 rep 类型生成边界，避免测试硬编码平台特定的 64 位最大值。
    const std::string maximum = std::to_string(std::numeric_limits<RetryRep>::max());

    MCPSseDecoder boundary_decoder = makeDecoder();
    const std::vector<MCPSseEvent> boundary_events = boundary_decoder.feed(std::string{"retry: "} + maximum + "\n\n");
    ASSERT_EQ(boundary_events.size(), 1U);
    ASSERT_TRUE(boundary_events[0].retry.has_value());
    EXPECT_EQ(*boundary_events[0].retry, std::chrono::milliseconds{3000});

    MCPSseDecoder overflow_decoder = makeDecoder();
    // 字符串进位只改变协议输入，不把不可表示的值构造成 C++ 整数。
    const std::string overflow = incrementDecimal(maximum);
    expectRuntimeErrorContains([&] { overflow_decoder.feed(std::string{"retry: "} + overflow + "\n\n"); },
                               "retry 超出整数表示范围");

    // 异常会清空溢出载荷，随后显式新事件仍可安全复用同一解码器。
    const std::vector<MCPSseEvent> recovered = overflow_decoder.feed("retry: 0\n\n");
    ASSERT_EQ(recovered.size(), 1U);
    ASSERT_TRUE(recovered[0].retry.has_value());
    EXPECT_EQ(*recovered[0].retry, std::chrono::milliseconds{0});
}

// data 字段之间始终保留一个换行，包括第一个字段本身为空的情况。
// 没有冒号的 data 等价于空值，仍通过 has_data 表示字段确实出现过。
TEST(MCPSseDecoderTest, PreservesEmptyAndMultilineData) {
    MCPSseDecoder decoder = makeDecoder();
    const std::vector<MCPSseEvent> events = decoder.feed("data\ndata: value\ndata:\n\n");

    // 三个 data 值依次为空、value、空，因此两个分隔换行都必须保留。
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(events[0].has_data);
    EXPECT_EQ(events[0].data, "\nvalue\n");
    EXPECT_FALSE(events[0].id.has_value());
}

// 三类硬失败分别覆盖事件预算、ID 预算和协议禁止的 NUL 字节。
// 每次失败都使用独立实例，避免一个故障遮蔽另一个边界条件。
TEST(MCPSseDecoderTest, RejectsSizeLimitsAndNulBytes) {
    MCPSseDecoder event_limited(12U, 64U, std::chrono::milliseconds{1000});
    expectRuntimeErrorContains([&] { event_limited.feed("data: 123456789\n\n"); }, "事件超过允许上限");

    MCPSseDecoder id_limited(128U, 4U, std::chrono::milliseconds{1000});
    expectRuntimeErrorContains([&] { id_limited.feed("id: 12345\n\n"); }, "事件 ID 超过允许上限");

    // push_back 确保零字节确实位于 std::string 中间，而非结束输入。
    MCPSseDecoder nul_decoder = makeDecoder();
    std::string invalid_nul = "data: before";
    invalid_nul.push_back('\0');
    invalid_nul.append("after\n\n");
    expectRuntimeErrorContains([&] { nul_decoder.feed(invalid_nul); }, "非法 NUL 字节");

    // 异常路径会清理缓存，随后仍可在明确的新事件边界上复用实例。
    const std::vector<MCPSseEvent> recovered = nul_decoder.feed("data: recovered\n\n");
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered[0].data, "recovered");
}

// EOF 不能代替空行提交事件；finish 丢弃残片后实例可以直接复用。
// reset 同样丢弃已经聚合的字段，并清除可能跨块等待的 CR 状态。
TEST(MCPSseDecoderTest, FinishAndResetDiscardIncompleteEvents) {
    MCPSseDecoder decoder = makeDecoder();
    EXPECT_TRUE(decoder.feed("id: partial\ndata: incomplete").empty());
    // 尾部已有完整字段但缺少事件空行，finish 仍必须返回空结果。
    EXPECT_TRUE(decoder.finish().empty());

    // finish 完成重置后，下一条独立流不能继承 partial 游标或 data。
    std::vector<MCPSseEvent> events = decoder.feed("id: after-finish\ndata: complete\n\n");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].data, "complete");

    EXPECT_TRUE(decoder.feed("id: before-reset\r").empty());
    // reset 发生在等待 CRLF 的中间状态，后续 LF 不得补全旧事件。
    decoder.reset();
    events = decoder.feed("id: after-reset\n\n");
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(events[0].id.has_value());
    EXPECT_EQ(*events[0].id, "after-reset");
    EXPECT_FALSE(events[0].has_data);
}

// 非法构造预算在任何输入到达前失败，避免产生永远无法满足的解码器。
TEST(MCPSseDecoderTest, RejectsInvalidLimits) {
    // 事件与 ID 的零预算无法容纳任何合法字段，必须立即拒绝。
    EXPECT_THROW(MCPSseDecoder(0U, 1U, std::chrono::milliseconds{1}), std::invalid_argument);
    EXPECT_THROW(MCPSseDecoder(1U, 0U, std::chrono::milliseconds{1}), std::invalid_argument);
    EXPECT_THROW(MCPSseDecoder(1U, 1U, std::chrono::milliseconds{-1}), std::invalid_argument);

    // retry 上限为零仍是合法策略，所有纯数字建议都被裁剪为零毫秒。
    MCPSseDecoder zero_retry(64U, 8U, std::chrono::milliseconds{0});
    const std::vector<MCPSseEvent> events = zero_retry.feed("retry: 9\n\n");
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(events[0].retry.has_value());
    EXPECT_EQ(*events[0].retry, std::chrono::milliseconds{0});

    // 空 ID 不是字段缺失，而是要求上层清除该流已经保存的恢复游标。
    // 该控制事件不携带 data，仍必须在事件空行到达时独立返回。
    // optional 必须保持 engaged，不能把空字符串错误转换成 nullopt。
    // 事件也不能继承前一条 retry，解码器不保存跨事件控制状态。
    const std::vector<MCPSseEvent> empty_id_events = zero_retry.feed("id:\n\n");
    ASSERT_EQ(empty_id_events.size(), 1U);
    ASSERT_TRUE(empty_id_events[0].id.has_value());
    EXPECT_TRUE(empty_id_events[0].id->empty());
    EXPECT_FALSE(empty_id_events[0].retry.has_value());
    EXPECT_FALSE(empty_id_events[0].has_data);
}

}  // namespace
