#pragma once

#include "ui/progress_box.hpp"
#include <functional>
#include <switch.h>

namespace sphaira::thread {

using ReadFunctionCallback = std::function<Result(void* data, s64 off, s64 size, u64* bytes_read)>;
using WriteFunctionCallback = std::function<Result(const void* data, s64 off, s64 size)>;
using PullFunctionCallback = std::function<Result(void* data, s64 size, u64* bytes_read)>;
using StartFunctionCallback = std::function<Result(PullFunctionCallback pull)>;

Result Transfer(ui::ProgressBox* pbox, s64 size, ReadFunctionCallback rfunc, WriteFunctionCallback wfunc);
Result TransferPull(ui::ProgressBox* pbox, s64 size, ReadFunctionCallback rfunc, StartFunctionCallback sfunc);

} // namespace sphaira::thread
