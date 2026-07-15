#pragma once

#include <chrono>

namespace aiSDK::detail {

// 所有公开超时最终都要转换为 steady_clock 的绝对截止时间。
// 配置允许使用 milliseconds 的完整正值范围，因此直接执行 now + timeout
// 可能使底层有符号 duration 回绕，并把超长等待误判为已经超时。
// 本辅助函数在接近 time_point 上界时饱和，保持“超长但有限”的保守语义。
// 提前不足一个 steady_clock tick 的饱和不会缩短任何实际可观察等待。
inline std::chrono::steady_clock::time_point saturatingSteadyDeadlineAfter(std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;

    // 只读取一次 now，保证上界比较与最终加法基于同一个线性化时刻。
    const Clock::time_point now = Clock::now();
    const Clock::duration remaining = Clock::time_point::max() - now;
    // 向毫秒截断最多提前一个底层 tick 进入饱和分支，不会发生越界加法。
    const auto remaining_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);

    // 调用方已在配置层拒绝零值和负值；此分支仍对内部复用保持安全。
    if(timeout <= std::chrono::milliseconds::zero()) {
        return now;
    }
    if(timeout >= remaining_milliseconds) {
        return Clock::time_point::max();
    }
    return now + timeout;
}

}  // namespace aiSDK::detail
