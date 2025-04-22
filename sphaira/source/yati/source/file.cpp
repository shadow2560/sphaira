#include "yati/source/file.hpp"

namespace sphaira::yati::source {

File::File(FsFileSystem* fs, const fs::FsPath& path) {
    m_open_result = fsFsOpenFile(fs, path, FsOpenMode_Read, std::addressof(m_file));
}

File::~File() {
    if (R_SUCCEEDED(GetOpenResult())) {
        fsFileClose(std::addressof(m_file));
    }
}

Result File::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    return fsFileRead(std::addressof(m_file), off, buf, size, 0, bytes_read);
}

} // namespace sphaira::yati::source
