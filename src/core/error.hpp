#pragma once

#include "teleop_ipc/error.hpp"

namespace teleop::core {

// 复用 IPC 的错误词汇：应用层不另起一套 Error/Result，避免在边界来回转换。
// 如果以后 core 要脱离 teleop_ipc，再拆出独立类型。
using Error = link::Error;
using ErrorCode = link::ErrorCode;

template <typename T>
using Result = link::Result<T>;

using link::error_code_name;
using link::is_would_block;

} // namespace teleop::core
