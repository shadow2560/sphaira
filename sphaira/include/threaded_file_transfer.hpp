#pragma once

#include "ui/progress_box.hpp"
#include <functional>
#include <switch.h>

namespace sphaira::thread {

using ReadCallback = std::function<Result(void* data, s64 off, s64 size, u64* bytes_read)>;
using WriteCallback = std::function<Result(const void* data, s64 off, s64 size)>;

// used for pull api
using PullCallback = std::function<Result(void* data, s64 size, u64* bytes_read)>;
using StartThreadCallback = std::function<Result(void)>;

// called when threads are started.
// call pull() to receive data.
using StartCallback = std::function<Result(PullCallback pull)>;

// same as above, but the callee must call start() in order to start threads.
// this is for convenience as there may be race conditions otherwise, such as the read thread
// trying to read from the pull callback before it is set.
using StartCallback2 = std::function<Result(StartThreadCallback start, PullCallback pull)>;

// reads data from rfunc into wfunc.
Result Transfer(ui::ProgressBox* pbox, s64 size, ReadCallback rfunc, WriteCallback wfunc);

// reads data from rfunc, pull data from provided pull() callback.
Result TransferPull(ui::ProgressBox* pbox, s64 size, ReadCallback rfunc, StartCallback sfunc);
Result TransferPull(ui::ProgressBox* pbox, s64 size, ReadCallback rfunc, StartCallback2 sfunc);

} // namespace sphaira::thread
