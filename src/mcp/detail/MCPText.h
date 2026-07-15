#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace aiSDK::detail {

// 两个辅助函数都只处理内存中的字节，不分配外部资源，也不抛协议异常。
// 调用方必须传入最终公开文本上限，不能用该函数替代配置验证。

// utf8PrefixLength 把字节上限回退到完整 UTF-8 码点边界。
// 输入来自已解析 JSON 或 SDK 固定文本，因此这里只负责边界，不重复做完整 UTF-8 校验。
inline std::size_t utf8PrefixLength(std::string_view text, std::size_t byte_limit) {
    // 未超限时直接返回原长度，避免扫描常见短错误文本。
    if(text.size() <= byte_limit) {
        return text.size();
    }
    std::size_t length = byte_limit;
    // UTF-8 continuation byte 形如 10xxxxxx，切点只能回退不能越过调用方上限。
    // text[length] 是第一个被排除字节；若它是 continuation byte，当前切点位于码点内部。
    while(length > 0U && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) {
        --length;
    }
    return length;
}

// sanitizeMCPErrorText 统一约束公开 MCPException 与 ToolResult 的文本。
// C0/DEL 控制字符不会进入日志或模型上下文；换行和制表保留以支持多段工具错误。
// 超限时追加固定中文标记；极小上限只返回该标记的完整 UTF-8 前缀。
inline std::string sanitizeMCPErrorText(std::string_view text, std::size_t max_bytes) {
    // 净化在截断之前执行，保证控制字节不会因刚好位于保留前缀而漏出。
    std::string sanitized;
    // reserve 仅按最终上限与输入较小者预留，不接受远端长度诱导的大额分配。
    sanitized.reserve(std::min(text.size(), max_bytes));
    for(const unsigned char character : text) {
        // 换行和制表对多段业务错误有语义，其余控制字节统一替换为空格。
        if((character < 0x20U && character != '\n' && character != '\t') || character == 0x7FU) {
            sanitized.push_back(' ');
        } else {
            sanitized.push_back(static_cast<char>(character));
        }
    }

    constexpr std::string_view truncation_marker = "……内容已截断";
    // 净化后仍在上限内时保持完整文本，不追加误导性的截断标记。
    if(sanitized.size() <= max_bytes) {
        return sanitized;
    }
    if(max_bytes <= truncation_marker.size()) {
        // 极小上限只能返回标记前缀，但仍严格停在完整 UTF-8 码点边界。
        return std::string(truncation_marker.substr(0U, utf8PrefixLength(truncation_marker, max_bytes)));
    }
    // 为完整标记预留空间后再截正文，最终结果不会超过 max_bytes。
    const std::size_t prefix_length = utf8PrefixLength(sanitized, max_bytes - truncation_marker.size());
    sanitized.resize(prefix_length);
    sanitized.append(truncation_marker.data(), truncation_marker.size());
    // 返回值可直接进入公开异常或 ToolResult，不需要调用方再次净化。
    return sanitized;
}

}  // namespace aiSDK::detail
