#pragma once

#include "base.hpp"
#include "fs.hpp"
#include <switch.h>
#include <memory>

namespace sphaira::yati::source {

struct File final : Base {
    File(FsFileSystem* fs, const fs::FsPath& path);
    File(const fs::FsPath& path);
    ~File();

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    std::unique_ptr<fs::Fs> m_fs{};
    fs::File m_file{};
};

} // namespace sphaira::yati::source
