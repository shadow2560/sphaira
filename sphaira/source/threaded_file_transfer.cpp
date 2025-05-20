#include "threaded_file_transfer.hpp"
#include "log.hpp"
#include "defines.hpp"
#include "app.hpp"

#include <vector>
#include <algorithm>
#include <cstring>

namespace sphaira::thread {
namespace {

constexpr u64 READ_BUFFER_MAX = 1024*1024*4;

struct ThreadBuffer {
    ThreadBuffer() {
        buf.reserve(READ_BUFFER_MAX);
    }

    std::vector<u8> buf;
    s64 off;
};

template<std::size_t Size>
struct RingBuf {
private:
    ThreadBuffer buf[Size]{};
    unsigned r_index{};
    unsigned w_index{};

    static_assert((sizeof(RingBuf::buf) & (sizeof(RingBuf::buf) - 1)) == 0, "Must be power of 2!");

public:
    void ringbuf_reset() {
        this->r_index = this->w_index;
    }

    unsigned ringbuf_capacity() const {
        return sizeof(this->buf) / sizeof(this->buf[0]);
    }

    unsigned ringbuf_size() const {
        return (this->w_index - this->r_index) % (ringbuf_capacity() * 2U);
    }

    unsigned ringbuf_free() const {
        return ringbuf_capacity() - ringbuf_size();
    }

    void ringbuf_push(std::vector<u8>& buf_in, s64 off_in) {
        auto& value = this->buf[this->w_index % ringbuf_capacity()];
        value.off = off_in;
        std::swap(value.buf, buf_in);

        this->w_index = (this->w_index + 1U) % (ringbuf_capacity() * 2U);
    }

    void ringbuf_pop(std::vector<u8>& buf_out, s64& off_out) {
        auto& value = this->buf[this->r_index % ringbuf_capacity()];
        off_out = value.off;
        std::swap(value.buf, buf_out);

        this->r_index = (this->r_index + 1U) % (ringbuf_capacity() * 2U);
    }
};

struct ThreadData {
    ThreadData(ui::ProgressBox* _pbox, s64 size, ReadFunctionCallback _rfunc, WriteFunctionCallback _wfunc);

    auto GetResults() -> Result;
    void WakeAllThreads();

    void SetReadResult(Result result) {
        read_result = result;
    }

    void SetWriteResult(Result result) {
        write_result = result;
    }

    void SetPullResult(Result result) {
        pull_result = result;
    }

    auto GetWriteOffset() const {
        return write_offset;
    }

    auto GetWriteSize() const {
        return write_size;
    }

    Result Pull(void* data, s64 size, u64* bytes_read);
    Result readFuncInternal();
    Result writeFuncInternal();

private:
    Result SetWriteBuf(std::vector<u8>& buf, s64 size);
    Result GetWriteBuf(std::vector<u8>& buf_out, s64& off_out);
    Result SetPullBuf(std::vector<u8>& buf, s64 size);
    Result GetPullBuf(void* data, s64 size, u64* bytes_read);

    Result Read(void* buf, s64 size, u64* bytes_read);

private:
    // these need to be copied
    ui::ProgressBox* pbox{};
    ReadFunctionCallback rfunc{};
    WriteFunctionCallback wfunc{};

    // these need to be created
    Mutex mutex{};
    Mutex pull_mutex{};

    CondVar can_read{};
    CondVar can_write{};
    CondVar can_pull{};
    CondVar can_pull_write{};

    RingBuf<2> write_buffers{};
    std::vector<u8> pull_buffer{};
    s64 pull_buffer_offset{};

    u64 read_buffer_size{};
    u64 max_buffer_size{};

    // these are shared between threads
    volatile s64 read_offset{};
    volatile s64 write_offset{};
    volatile s64 write_size{};

    volatile Result read_result{};
    volatile Result write_result{};
    volatile Result pull_result{};
};

ThreadData::ThreadData(ui::ProgressBox* _pbox, s64 size, ReadFunctionCallback _rfunc, WriteFunctionCallback _wfunc)
: pbox{_pbox}, rfunc{_rfunc}, wfunc{_wfunc} {
    mutexInit(std::addressof(mutex));
    mutexInit(std::addressof(pull_mutex));

    condvarInit(std::addressof(can_read));
    condvarInit(std::addressof(can_write));
    condvarInit(std::addressof(can_pull));
    condvarInit(std::addressof(can_pull_write));

    write_size = size;
    read_buffer_size = READ_BUFFER_MAX;
    max_buffer_size = READ_BUFFER_MAX;
}

auto ThreadData::GetResults() -> Result {
    R_UNLESS(!pbox->ShouldExit(), 0x1);
    R_TRY(read_result);
    R_TRY(write_result);
    R_TRY(pull_result);
    R_SUCCEED();
}

void ThreadData::WakeAllThreads() {
    condvarWakeAll(std::addressof(can_read));
    condvarWakeAll(std::addressof(can_write));
    condvarWakeAll(std::addressof(can_pull));
    condvarWakeAll(std::addressof(can_pull_write));

    mutexUnlock(std::addressof(mutex));
    mutexUnlock(std::addressof(pull_mutex));
}

Result ThreadData::SetWriteBuf(std::vector<u8>& buf, s64 size) {
    buf.resize(size);

    mutexLock(std::addressof(mutex));
    if (!write_buffers.ringbuf_free()) {
        R_TRY(condvarWait(std::addressof(can_read), std::addressof(mutex)));
    }

    ON_SCOPE_EXIT(mutexUnlock(std::addressof(mutex)));
    R_TRY(GetResults());
    write_buffers.ringbuf_push(buf, 0);
    return condvarWakeOne(std::addressof(can_write));
}

Result ThreadData::GetWriteBuf(std::vector<u8>& buf_out, s64& off_out) {
    mutexLock(std::addressof(mutex));
    if (!write_buffers.ringbuf_size()) {
        R_TRY(condvarWait(std::addressof(can_write), std::addressof(mutex)));
    }

    ON_SCOPE_EXIT(mutexUnlock(std::addressof(mutex)));
    R_TRY(GetResults());
    write_buffers.ringbuf_pop(buf_out, off_out);
    return condvarWakeOne(std::addressof(can_read));
}

Result ThreadData::SetPullBuf(std::vector<u8>& buf, s64 size) {
    buf.resize(size);

    mutexLock(std::addressof(pull_mutex));
    if (!pull_buffer.empty()) {
        R_TRY(condvarWait(std::addressof(can_pull_write), std::addressof(pull_mutex)));
    }

    ON_SCOPE_EXIT(mutexUnlock(std::addressof(pull_mutex)));
    R_TRY(GetResults());

    pull_buffer.swap(buf);
    return condvarWakeOne(std::addressof(can_pull));
}

Result ThreadData::GetPullBuf(void* data, s64 size, u64* bytes_read) {
    mutexLock(std::addressof(pull_mutex));
    if (pull_buffer.empty()) {
        R_TRY(condvarWait(std::addressof(can_pull), std::addressof(pull_mutex)));
    }

    ON_SCOPE_EXIT(mutexUnlock(std::addressof(pull_mutex)));
    R_TRY(GetResults());

    *bytes_read = size = std::min<s64>(size, pull_buffer.size() - pull_buffer_offset);
    std::memcpy(data, pull_buffer.data() + pull_buffer_offset, size);
    pull_buffer_offset += size;

    if (pull_buffer_offset == pull_buffer.size()) {
        pull_buffer_offset = 0;
        pull_buffer.clear();
        return condvarWakeOne(std::addressof(can_pull_write));
    } else {
        R_SUCCEED();
    }
}

Result ThreadData::Read(void* buf, s64 size, u64* bytes_read) {
    size = std::min<s64>(size, write_size - read_offset);
    const auto rc = rfunc(buf, read_offset, size, bytes_read);
    read_offset += *bytes_read;
    return rc;
}

Result ThreadData::Pull(void* data, s64 size, u64* bytes_read) {
    return GetPullBuf(data, size, bytes_read);
}

// read thread reads all data from the source
Result ThreadData::readFuncInternal() {
    // the main buffer which data is read into.
    std::vector<u8> buf;
    buf.reserve(this->max_buffer_size);

    while (this->read_offset < this->write_size && R_SUCCEEDED(this->GetResults())) {
        // read more data
        s64 read_size = this->read_buffer_size;

        u64 bytes_read{};
        buf.resize(read_size);
        R_TRY(this->Read(buf.data(), read_size, std::addressof(bytes_read)));
        auto buf_size = bytes_read;

        R_TRY(this->SetWriteBuf(buf, buf_size));
    }

    log_write("read success\n");
    R_SUCCEED();
}

// write thread writes data to the nca placeholder.
Result ThreadData::writeFuncInternal() {
    std::vector<u8> buf;
    buf.reserve(this->max_buffer_size);

    while (this->write_offset < this->write_size && R_SUCCEEDED(this->GetResults())) {
        s64 dummy_off;
        R_TRY(this->GetWriteBuf(buf, dummy_off));
        const auto size = buf.size();

        if (!this->wfunc) {
            R_TRY(this->SetPullBuf(buf, buf.size()));
        } else {
            R_TRY(this->wfunc(buf.data(), this->write_offset, buf.size()));
        }

        this->write_offset += size;
    }

    log_write("finished write thread!\n");
    R_SUCCEED();
}

void readFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetReadResult(t->readFuncInternal());
    log_write("read thread returned now\n");
}

void writeFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetWriteResult(t->writeFuncInternal());
    log_write("write thread returned now\n");
}

auto GetAlternateCore(int id) {
    return id == 1 ? 2 : 1;
}

Result TransferInternal(ui::ProgressBox* pbox, s64 size, ReadFunctionCallback rfunc, WriteFunctionCallback wfunc, StartFunctionCallback sfunc) {
    App::SetAutoSleepDisabled(true);
    ON_SCOPE_EXIT(App::SetAutoSleepDisabled(false));

    const auto WRITE_THREAD_CORE = sfunc ? pbox->GetCpuId() : GetAlternateCore(pbox->GetCpuId());
    const auto READ_THREAD_CORE = GetAlternateCore(WRITE_THREAD_CORE);

    ThreadData t_data{pbox, size, rfunc, wfunc};

    Thread t_read{};
    R_TRY(threadCreate(&t_read, readFunc, std::addressof(t_data), nullptr, 1024*64, 0x20, READ_THREAD_CORE));
    ON_SCOPE_EXIT(threadClose(&t_read));

    Thread t_write{};
    R_TRY(threadCreate(&t_write, writeFunc, std::addressof(t_data), nullptr, 1024*64, 0x20, WRITE_THREAD_CORE));
    ON_SCOPE_EXIT(threadClose(&t_write));

    log_write("starting threads\n");
    R_TRY(threadStart(std::addressof(t_read)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_read)));

    R_TRY(threadStart(std::addressof(t_write)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_write)));

    if (sfunc) {
        t_data.SetPullResult(sfunc([&](void* data, s64 size, u64* bytes_read) -> Result {
            R_TRY(t_data.GetResults());
            return t_data.Pull(data, size, bytes_read);
        }));
    } else {
        while (t_data.GetWriteOffset() != t_data.GetWriteSize() && R_SUCCEEDED(t_data.GetResults())) {
            pbox->UpdateTransfer(t_data.GetWriteOffset(), t_data.GetWriteSize());
            svcSleepThread(1e+6);
        }
    }

    // wait for all threads to close.
    log_write("waiting for threads to close\n");
    for (;;) {
        t_data.WakeAllThreads();
        pbox->Yield();

        if (R_FAILED(waitSingleHandle(t_read.handle, 1000))) {
            continue;
        } else if (R_FAILED(waitSingleHandle(t_write.handle, 1000))) {
            continue;
        }
        break;
    }
    log_write("threads closed\n");

    // if any of the threads failed, wake up all threads so they can exit.
    if (R_FAILED(t_data.GetResults())) {
        log_write("some reads failed, waking threads\n");
        log_write("returning due to fail\n");
        return t_data.GetResults();
    }

    return t_data.GetResults();
}

} // namespace

Result Transfer(ui::ProgressBox* pbox, s64 size, ReadFunctionCallback rfunc, WriteFunctionCallback wfunc) {
    return TransferInternal(pbox, size, rfunc, wfunc, nullptr);
}

Result TransferPull(ui::ProgressBox* pbox, s64 size, ReadFunctionCallback rfunc, StartFunctionCallback sfunc) {
    return TransferInternal(pbox, size, rfunc, nullptr, sfunc);
}

} // namespace::thread
