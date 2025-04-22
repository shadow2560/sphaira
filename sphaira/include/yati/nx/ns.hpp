#pragma once

#include <switch.h>
#include "ncm.hpp"

namespace sphaira::ns {

enum ApplicationRecordType {
    // installed
    ApplicationRecordType_Installed       = 0x3,
    // application is gamecard, but gamecard isn't insterted
    ApplicationRecordType_GamecardMissing = 0x5,
    // archived
    ApplicationRecordType_Archived        = 0xB,
};

Result PushApplicationRecord(Service* srv, u64 tid, const ncm::ContentStorageRecord* records, u32 count);
Result ListApplicationRecordContentMeta(Service* srv, u64 offset, u64 tid, ncm::ContentStorageRecord* out_records, u32 count, s32* entries_read);
Result DeleteApplicationRecord(Service* srv, u64 tid);
Result InvalidateApplicationControlCache(Service* srv, u64 tid);

} // namespace sphaira::ns
