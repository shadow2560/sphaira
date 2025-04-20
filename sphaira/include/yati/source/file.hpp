#pragma once

#include "base.hpp"
#include "fs.hpp"
#include <switch.h>

namespace sphaira::yati::source {

struct File final : Base {
    File(FsFileSystem* fs, const fs::FsPath& path);
    ~File();

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    FsFile m_file{};
};

} // namespace sphaira::yati::source
