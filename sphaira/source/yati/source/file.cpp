#include "yati/source/file.hpp"

namespace sphaira::yati::source {

File::File(FsFileSystem* fs, const fs::FsPath& path) {
    if (fs) {
        m_fs = std::make_unique<fs::FsNative>(fs, false);
    } else {
        m_fs = std::make_unique<fs::FsStdio>();
    }

    m_open_result = m_fs->OpenFile(path, FsOpenMode_Read, std::addressof(m_file));
}

File::File(const fs::FsPath& path) : File{nullptr, path} {

}

File::~File() {
    if (R_SUCCEEDED(GetOpenResult())) {
        m_fs->FileClose(std::addressof(m_file));
    }
}

Result File::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    return m_fs->FileRead(std::addressof(m_file), off, buf, size, 0, bytes_read);
}

} // namespace sphaira::yati::source
