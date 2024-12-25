#pragma once

#include <switch.h>

struct FtpVfsFile {
    FsFile fd;
    s64 off;
    s64 buf_off;
    s64 buf_size;
    bool is_write;
    bool is_valid;
    u8 buf[1024 * 1024 * 1];
};

struct FtpVfsDir {
    FsDir dir;
    bool is_valid;
};

struct FtpVfsDirEntry {
    FsDirectoryEntry buf;
};

#ifdef __cplusplus

namespace sphaira::ftpsrv {

bool Init();
void Exit();

} // namespace sphaira::ftpsrv

#endif // __cplusplus
