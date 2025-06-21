#pragma once

// #include <optional>
// #include <variant>
// #include <list>
// #include <string>
#include "fs.hpp"
#include <optional>
#include <span>
#include <vector>
#include <memory>
#include <functional>
#include <switch.h>

namespace sphaira::title {

constexpr u32 ContentMetaTypeToContentFlag(u8 meta_type) {
    if (meta_type & 0x80) {
        return 1 << (meta_type - 0x80);
    }

    return 0;
}

enum ContentFlag {
    ContentFlag_Application = ContentMetaTypeToContentFlag(NcmContentMetaType_Application),
    ContentFlag_Patch = ContentMetaTypeToContentFlag(NcmContentMetaType_Patch),
    ContentFlag_AddOnContent = ContentMetaTypeToContentFlag(NcmContentMetaType_AddOnContent),
    ContentFlag_DataPatch = ContentMetaTypeToContentFlag(NcmContentMetaType_DataPatch),

    // nca locations where a control.nacp can exist.
    ContentFlag_Nacp = ContentFlag_Application | ContentFlag_Patch,
    // all of the above.
    ContentFlag_All = ContentFlag_Application | ContentFlag_Patch | ContentFlag_AddOnContent | ContentFlag_DataPatch,
};

enum class NacpLoadStatus {
    // not yet attempted to be loaded.
    None,
    // started loading.
    Progress,
    // loaded, ready to parse.
    Loaded,
    // failed to load, do not attempt to load again!
    Error,
};

struct ThreadResultData {
    u64 id{};
    std::shared_ptr<NsApplicationControlData> control{};
    u64 jpeg_size{};
    NacpLanguageEntry lang{};
    NacpLoadStatus status{NacpLoadStatus::None};
};

using MetaEntries = std::vector<NsApplicationContentMetaStatus>;

// starts background thread.
Result Init();
void Exit();

// adds new entry to queue.
void Push(u64 app_id);
// adds array of entries to queue.
void Push(std::span<const u64> app_ids);

#if 0
// removes entry from the queue into out.
void Pop(u64 app_id, std::vector<ThreadResultData>& out);
// removes array of entries from the queue into out.
void Pop(std::span<const u64> app_ids, std::vector<ThreadResultData>& out);
// removes all entries from the queue into out.
void Pop(std::vector<ThreadResultData>& out);
#endif

// gets entry without removing it from the queue.
auto Get(u64 app_id) -> std::optional<ThreadResultData>;
// gets array of entries without removing it from the queue.
void Get(std::span<const u64> app_ids, std::vector<ThreadResultData>& out);

auto GetNcmCs(u8 storage_id) -> NcmContentStorage&;
auto GetNcmDb(u8 storage_id) -> NcmContentMetaDatabase&;

// gets all meta entries for an id.
Result GetMetaEntries(u64 id, MetaEntries& out, u32 flags = ContentFlag_All);

// returns the nca path of a control nca.
Result GetControlPathFromStatus(const NsApplicationContentMetaStatus& status, u64* out_program_id, fs::FsPath* out_path);

// single threaded title info fetch.
auto LoadControlEntry(u64 id, bool* cached = nullptr) -> ThreadResultData;

// taken from nxdumptool.
void utilsReplaceIllegalCharacters(char *str, bool ascii_only);

} // namespace sphaira::title
