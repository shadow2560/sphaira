#include "yati/source/stream_file.hpp"
#include "log.hpp"

namespace sphaira::yati::source {

StreamFile::StreamFile(FsFileSystem* fs, const fs::FsPath& path) {
    if (fs) {
        m_fs = std::make_unique<fs::FsNative>(fs, false);
    } else {
        m_fs = std::make_unique<fs::FsStdio>();
    }

    m_open_result = m_fs->OpenFile(path, FsOpenMode_Read, std::addressof(m_file));
}

StreamFile::StreamFile(const fs::FsPath& path) : StreamFile{nullptr, path} {

}

StreamFile::~StreamFile() {
    if (R_SUCCEEDED(GetOpenResult())) {
        m_fs->FileClose(std::addressof(m_file));
    }
}

Result StreamFile::ReadChunk(void* buf, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    const auto rc = m_fs->FileRead(std::addressof(m_file), m_offset, buf, size, 0, bytes_read);
    m_offset += *bytes_read;
    return rc;
}

} // namespace sphaira::yati::source
