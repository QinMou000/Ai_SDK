#pragma once

#include <optional>
#include <utility>

#include "core/Error.h"

namespace aiSDK {

// Result 先提供轻量结果包装，
// 后续真实实现时可以继续沿用这一层错误边界。
template <typename T>
struct Result {
    std::optional<T> value;
    std::optional<Error> error;

    bool ok() const {
        return value.has_value() && !error.has_value();
    }

    static Result<T> success(T result) {
        return Result<T>{std::move(result), std::nullopt};
    }

    static Result<T> failure(Error failure_error) {
        return Result<T>{std::nullopt, std::move(failure_error)};
    }
};

}  // namespace aiSDK