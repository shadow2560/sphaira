#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "location.hpp"
#include "image.hpp"
#include "threaded_file_transfer.hpp"
#include "minizip_helper.hpp"

#include "ui/menus/save_menu.hpp"
#include "ui/menus/filebrowser.hpp"

#include "ui/sidebar.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"

#include "yati/nx/ncm.hpp"
#include "yati/nx/nca.hpp"

#include <utility>
#include <cstring>
#include <algorithm>
#include <minIni.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <nxtc.h>

namespace sphaira::ui::menu::save {
namespace {

constexpr int THREAD_PRIO = PRIO_PREEMPTIVE;
constexpr int THREAD_CORE = 1;

constexpr u32 NX_SAVE_META_MAGIC = 0x4A4B5356; // JKSV
constexpr u32 NX_SAVE_META_VERSION = 1;
constexpr const char* NX_SAVE_META_NAME = ".nx_save_meta.bin";

// https://github.com/J-D-K/JKSV/issues/264#issuecomment-2618962807
struct NXSaveMeta {
    u32 magic{}; // NX_SAVE_META_MAGIC
    u32 version{}; // NX_SAVE_META_VERSION
    FsSaveDataAttribute attr{}; // FsSaveDataExtraData::attr
    u64 owner_id{}; // FsSaveDataExtraData::owner_id
    u64 timestamp{}; // FsSaveDataExtraData::timestamp
    u32 flags{}; // FsSaveDataExtraData::flags
    u32 unk_x54{}; // FsSaveDataExtraData::unk_x54
    s64 data_size{}; // FsSaveDataExtraData::data_size
    s64 journal_size{}; // FsSaveDataExtraData::journal_size
    u64 commit_id{}; // FsSaveDataExtraData::commit_id
    u64 raw_size{}; // FsSaveDataInfo::size
};
static_assert(sizeof(NXSaveMeta) == 128);

// taken from nxtc
constexpr u8 g_nacpLangTable[SetLanguage_Total] = {
    [SetLanguage_JA]     =  2,
    [SetLanguage_ENUS]   =  0,
    [SetLanguage_FR]     =  3,
    [SetLanguage_DE]     =  4,
    [SetLanguage_IT]     =  7,
    [SetLanguage_ES]     =  6,
    [SetLanguage_ZHCN]   = 14,
    [SetLanguage_KO]     = 12,
    [SetLanguage_NL]     =  8,
    [SetLanguage_PT]     = 10,
    [SetLanguage_RU]     = 11,
    [SetLanguage_ZHTW]   = 13,
    [SetLanguage_ENGB]   =  1,
    [SetLanguage_FRCA]   =  9,
    [SetLanguage_ES419]  =  5,
    [SetLanguage_ZHHANS] = 14,
    [SetLanguage_ZHHANT] = 13,
    [SetLanguage_PTBR]   = 15
};

auto GetNacpLangEntryIndex() -> u8 {
    SetLanguage lang{SetLanguage_ENUS};
    nxtcGetCacheLanguage(&lang);
    return g_nacpLangTable[lang];
}

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
    ContentFlag_All = ContentFlag_Application | ContentFlag_Patch | ContentFlag_AddOnContent | ContentFlag_DataPatch,
};

struct NcmEntry {
    const NcmStorageId storage_id;
    NcmContentStorage cs{};
    NcmContentMetaDatabase db{};

    void Open() {
        if (R_FAILED(ncmOpenContentMetaDatabase(std::addressof(db), storage_id))) {
            log_write("\tncmOpenContentMetaDatabase() failed. storage_id: %u\n", storage_id);
        } else {
            log_write("\tncmOpenContentMetaDatabase() success. storage_id: %u\n", storage_id);
        }

        if (R_FAILED(ncmOpenContentStorage(std::addressof(cs), storage_id))) {
            log_write("\tncmOpenContentStorage() failed. storage_id: %u\n", storage_id);
        } else {
            log_write("\tncmOpenContentStorage() success. storage_id: %u\n", storage_id);
        }
    }

    void Close() {
        ncmContentMetaDatabaseClose(std::addressof(db));
        ncmContentStorageClose(std::addressof(cs));

        db = {};
        cs = {};
    }
};

constinit NcmEntry ncm_entries[] = {
    // on memory, will become invalid on the gamecard being inserted / removed.
    { NcmStorageId_GameCard },
    // normal (save), will remain valid.
    { NcmStorageId_BuiltInUser },
    { NcmStorageId_SdCard },
};

auto& GetNcmEntry(u8 storage_id) {
    auto it = std::ranges::find_if(ncm_entries, [storage_id](auto& e){
        return storage_id == e.storage_id;
    });

    if (it == std::end(ncm_entries)) {
        log_write("unable to find valid ncm entry: %u\n", storage_id);
        return ncm_entries[0];
    }

    return *it;
}

auto& GetNcmCs(u8 storage_id) {
    return GetNcmEntry(storage_id).cs;
}

auto& GetNcmDb(u8 storage_id) {
    return GetNcmEntry(storage_id).db;
}

using MetaEntries = std::vector<NsApplicationContentMetaStatus>;

Result GetMetaEntries(u64 id, MetaEntries& out, u32 flags = ContentFlag_All) {
    for (s32 i = 0; ; i++) {
        s32 count;
        NsApplicationContentMetaStatus status;
        R_TRY(nsListApplicationContentMetaStatus(id, i, &status, 1, &count));

        if (!count) {
            break;
        }

        if (flags & ContentMetaTypeToContentFlag(status.meta_type)) {
            out.emplace_back(status);
        }
    }

    R_SUCCEED();
}

// also sets the status to error.
void FakeNacpEntry(ThreadResultData& e) {
    e.status = NacpLoadStatus::Error;
    // fake the nacp entry
    std::strcpy(e.lang.name, "Corrupted");
    std::strcpy(e.lang.author, "Corrupted");
    e.control.reset();
}

bool LoadControlImage(Entry& e) {
    if (!e.image && e.control) {
        ON_SCOPE_EXIT(e.control.reset());

        TimeStamp ts;
        const auto image = ImageLoadFromMemory({e.control->icon, e.jpeg_size}, ImageFlag_JPEG);
        if (!image.data.empty()) {
            e.image = nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
            log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
            return true;
        }
    }

    return false;
}

Result GetControlPathFromStatus(const NsApplicationContentMetaStatus& status, u64* out_program_id, fs::FsPath* out_path) {
    const auto& ee = status;
    if (ee.storageID != NcmStorageId_SdCard && ee.storageID != NcmStorageId_BuiltInUser && ee.storageID != NcmStorageId_GameCard) {
        return 0x1;
    }

    auto& db = GetNcmDb(ee.storageID);
    auto& cs = GetNcmCs(ee.storageID);

    NcmContentMetaKey key;
    R_TRY(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, ee.application_id));

    NcmContentId content_id;
    R_TRY(ncmContentMetaDatabaseGetContentIdByType(&db, &content_id, &key, NcmContentType_Control));

    R_TRY(ncmContentStorageGetProgramId(&cs, out_program_id, &content_id, FsContentAttributes_All));

    R_TRY(ncmContentStorageGetPath(&cs, out_path->s, sizeof(*out_path), &content_id));
    R_SUCCEED();
}

Result LoadControlManual(u64 id, ThreadResultData& data) {
    TimeStamp ts;

    MetaEntries entries;
    R_TRY(GetMetaEntries(id, entries));
    R_UNLESS(!entries.empty(), 0x1);

    u64 program_id;
    fs::FsPath path;
    R_TRY(GetControlPathFromStatus(entries.back(), &program_id, &path));

    std::vector<u8> icon;
    R_TRY(nca::ParseControl(path, program_id, &data.control->nacp.lang[GetNacpLangEntryIndex()], sizeof(NacpLanguageEntry), &icon));
    std::memcpy(data.control->icon, icon.data(), icon.size());

    data.jpeg_size = icon.size();
    log_write("\t\t[manual control] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());

    R_SUCCEED();
}

auto LoadControlEntry(u64 id) -> ThreadResultData {
    ThreadResultData data{};
    data.id = id;
    data.control = std::make_shared<NsApplicationControlData>();
    data.status = NacpLoadStatus::Error;

    bool manual_load = true;
    if (hosversionBefore(20,0,0)) {
        TimeStamp ts;
        u64 actual_size;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, id, data.control.get(), sizeof(NsApplicationControlData), &actual_size))) {
            manual_load = false;
            data.jpeg_size = actual_size - sizeof(NacpStruct);
            log_write("\t\t[ns control cache] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
        }
    }

    if (manual_load) {
        manual_load = R_SUCCEEDED(LoadControlManual(id, data));
    }

    Result rc{};
    if (!manual_load) {
        TimeStamp ts;
        u64 actual_size;
        if (R_SUCCEEDED(rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, data.control.get(), sizeof(NsApplicationControlData), &actual_size))) {
            data.jpeg_size = actual_size - sizeof(NacpStruct);
            log_write("\t\t[ns control storage] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
        }
    }

    if (R_SUCCEEDED(rc)) {
        data.lang = data.control->nacp.lang[GetNacpLangEntryIndex()];
        data.status = NacpLoadStatus::Loaded;
    }

    if (R_FAILED(rc)) {
        FakeNacpEntry(data);
    }

    return data;
}

void LoadResultIntoEntry(Entry& e, const ThreadResultData& result) {
    e.status = result.status;
    e.control = result.control;
    e.jpeg_size= result.jpeg_size;
    e.lang = result.lang;
    e.status = result.status;
}

void LoadControlEntry(Entry& e, bool force_image_load = false) {
    if (e.status == NacpLoadStatus::None) {
        const auto result = LoadControlEntry(e.application_id);
        LoadResultIntoEntry(e, result);
    }

    if (force_image_load && e.status == NacpLoadStatus::Loaded) {
        LoadControlImage(e);
    }
}

// taken from nxdumptool.
void utilsReplaceIllegalCharacters(char *str, bool ascii_only)
{
    static const char g_illegalFileSystemChars[] = "\\/:*?\"<>|";

    size_t str_size = 0, cur_pos = 0;

    if (!str || !(str_size = strlen(str))) return;

    u8 *ptr1 = (u8*)str, *ptr2 = ptr1;
    ssize_t units = 0;
    u32 code = 0;
    bool repl = false;

    while(cur_pos < str_size)
    {
        units = decode_utf8(&code, ptr1);
        if (units < 0) break;

        if (code < 0x20 || (!ascii_only && code == 0x7F) || (ascii_only && code >= 0x7F) || \
            (units == 1 && memchr(g_illegalFileSystemChars, (int)code, std::size(g_illegalFileSystemChars))))
        {
            if (!repl)
            {
                *ptr2++ = '_';
                repl = true;
            }
        } else {
            if (ptr2 != ptr1) memmove(ptr2, ptr1, (size_t)units);
            ptr2 += units;
            repl = false;
        }

        ptr1 += units;
        cur_pos += (size_t)units;
    }

    *ptr2 = '\0';
}

struct HashStr {
    char str[0x21];
};

HashStr hexIdToStr(auto id) {
    HashStr str{};
    const auto id_lower = std::byteswap(*(u64*)id.c);
    const auto id_upper = std::byteswap(*(u64*)(id.c + 0x8));
    std::snprintf(str.str, 0x21, "%016lx%016lx", id_lower, id_upper);
    return str;
}

auto BuildSaveName(const Entry& e) -> fs::FsPath {
    fs::FsPath name_buf = e.GetName();
    utilsReplaceIllegalCharacters(name_buf, true);
    return name_buf;
}

auto BuildSaveBasePath(const Entry& e) -> fs::FsPath {
    const auto name = BuildSaveName(e);
    return fs::AppendPath("/dumps/SAVE/", name);
}

auto BuildSavePath(const AccountEntry& acc, const Entry& e, bool is_auto) -> fs::FsPath {
    const auto t = std::time(NULL);
    const auto tm = std::localtime(&t);
    const auto base = BuildSaveBasePath(e);

    fs::FsPath name_buf;
    if (is_auto) {
        std::snprintf(name_buf, sizeof(name_buf), "AUTO - %s", acc.base.nickname);
    } else {
        std::snprintf(name_buf, sizeof(name_buf), "%s", acc.base.nickname);
    }
    utilsReplaceIllegalCharacters(name_buf, true);

    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s - %u.%02u.%02u @ %02u.%02u.%02u.zip", base.s, name_buf.s, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    return path;
}

Result RestoreSaveInternal(ProgressBox* pbox, const Entry& e, const fs::FsPath& path) {
    pbox->SetTitle(e.GetName());
    if (e.image) {
        pbox->SetImage(e.image);
    } else if (e.control && e.jpeg_size) {
        pbox->SetImageDataConst({e.control->icon, e.jpeg_size});
    } else {
        pbox->SetImage(0);
    }

    const auto save_data_space_id = (FsSaveDataSpaceId)e.save_data_space_id;

    // try and get the journal and data size.
    FsSaveDataExtraData extra{};
    R_TRY(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), save_data_space_id, e.save_data_id));

    log_write("restoring save: %s\n", path.s);
    zlib_filefunc64_def file_func;
    mz::FileFuncStdio(&file_func);

    auto zfile = unzOpen2_64(path, &file_func);
    R_UNLESS(zfile, 0x1);
    ON_SCOPE_EXIT(unzClose(zfile));
    log_write("opened zip\n");

    bool has_meta{};
    NXSaveMeta meta{};

    // get manifest
    if (UNZ_END_OF_LIST_OF_FILE != unzLocateFile(zfile, NX_SAVE_META_NAME, 0)) {
        log_write("found meta file\n");
        if (UNZ_OK == unzOpenCurrentFile(zfile)) {
            log_write("opened meta file\n");
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            const auto len = unzReadCurrentFile(zfile, &meta, sizeof(meta));
            if (len == sizeof(meta) && meta.magic == NX_SAVE_META_MAGIC && meta.version == NX_SAVE_META_VERSION) {
                has_meta = true;
                log_write("loaded meta!\n");
            }
        }
    }

    if (has_meta) {
        log_write("extending save file\n");
        R_TRY(fsExtendSaveDataFileSystem(save_data_space_id, e.save_data_id, meta.data_size, meta.journal_size));
        log_write("extended save file\n");
    } else {
        log_write("doing manual meta parse\n");
        s64 total_size{};

        // todo:: manually calculate / guess the save size.
        unz_global_info64 ginfo;
        R_UNLESS(UNZ_OK == unzGetGlobalInfo64(zfile, &ginfo), 0x1);
        R_UNLESS(UNZ_OK == unzGoToFirstFile(zfile), 0x1);

        for (s64 i = 0; i < ginfo.number_entry; i++) {
            R_TRY(pbox->ShouldExitResult());

            if (i > 0) {
                R_UNLESS(UNZ_OK == unzGoToNextFile(zfile), 0x1);
            }

            R_UNLESS(UNZ_OK == unzOpenCurrentFile(zfile), 0x1);
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            fs::FsPath name;
            R_UNLESS(UNZ_OK == unzGetCurrentFileInfo64(zfile, &info, name, sizeof(name), 0, 0, 0, 0), 0x1);

            if (name == NX_SAVE_META_NAME) {
                continue;
            }
            total_size += info.uncompressed_size;
        }

        // TODO: untested, should work tho.
        const auto rounded_size = total_size + (total_size % extra.journal_size);
        log_write("extendeing manual meta parse\n");
        R_TRY(fsExtendSaveDataFileSystem(save_data_space_id, e.save_data_id, rounded_size, extra.journal_size));
        log_write("extended manual meta parse\n");
    }

    FsSaveDataAttribute attr{};
    attr.application_id = e.application_id;
    attr.uid = e.uid;
    attr.system_save_data_id = e.system_save_data_id;
    attr.save_data_type = e.save_data_type;
    attr.save_data_rank = e.save_data_rank;
    attr.save_data_index = e.save_data_index;

    // try and open the save file system.
    fs::FsNativeSave save_fs{save_data_space_id, &attr, false};
    R_TRY(save_fs.GetFsOpenResult());

    log_write("opened save file\n");
    // restore save data from zip.
    R_TRY(thread::TransferUnzipAll(pbox, zfile, &save_fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
        // skip restoring the meta file.
        if (name == NX_SAVE_META_NAME) {
            log_write("skipping meta\n");
            return false;
        }

        // restore everything else.
        log_write("restoring: %s\n", path.s);

        // commit after every save otherwise FsError_MappingTableFull is thrown.
        R_TRY(save_fs.Commit());
        return true;
    }));

    log_write("finished, doing commit\n");
    R_TRY(save_fs.Commit());
    R_SUCCEED();
}

Result BackupSaveInternal(ProgressBox* pbox, const AccountEntry& acc, const Entry& e, bool compressed, bool is_auto = false) {
    pbox->SetTitle(e.GetName());
    if (e.image) {
        pbox->SetImage(e.image);
    } else if (e.control && e.jpeg_size) {
        pbox->SetImageDataConst({e.control->icon, e.jpeg_size});
    } else {
        pbox->SetImage(0);
    }

    const auto save_data_space_id = (FsSaveDataSpaceId)e.save_data_space_id;

    // try and get the journal and data size.
    FsSaveDataExtraData extra{};
    R_TRY(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), save_data_space_id, e.save_data_id));

    FsSaveDataAttribute attr{};
    attr.application_id = e.application_id;
    attr.uid = e.uid;
    attr.system_save_data_id = e.system_save_data_id;
    attr.save_data_type = e.save_data_type;
    attr.save_data_rank = e.save_data_rank;
    attr.save_data_index = e.save_data_index;

    // try and open the save file system
    fs::FsNativeSave save_fs{save_data_space_id, &attr, true};
    R_TRY(save_fs.GetFsOpenResult());

    // get a list of collections.
    filebrowser::FsDirCollections collections;
    R_TRY(filebrowser::FsView::get_collections(&save_fs, "/", "", collections));

    // the save file may be empty, this isn't an error, but we exit early.
    R_UNLESS(!collections.empty(), 0x0);

    // we will actually store this to the dump locations, eventually.
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    const auto t = std::time(NULL);
    const auto tm = std::localtime(&t);

    // pre-calculate the time rather than calculate it in the loop.
    zip_fileinfo zip_info_default{};
    zip_info_default.tmz_date.tm_sec = tm->tm_sec;
    zip_info_default.tmz_date.tm_min = tm->tm_min;
    zip_info_default.tmz_date.tm_hour = tm->tm_hour;
    zip_info_default.tmz_date.tm_mday = tm->tm_mday;
    zip_info_default.tmz_date.tm_mon = tm->tm_mon;
    zip_info_default.tmz_date.tm_year = tm->tm_year;

    const auto path = BuildSavePath(acc, e, is_auto);
    const auto temp_path = path + ".temp";

    fs.CreateDirectoryRecursivelyWithPath(temp_path);
    ON_SCOPE_EXIT(fs.DeleteFile(temp_path));

    // zip to memory if less than 1GB and not applet mode.
    // TODO: use my mmz code from ftpsrv to stream zip creation.
    // this will allow for zipping to memory and flushing every X bytes
    // such as flushing every 8MB.
    const auto file_download = App::IsApplet() || e.size >= 1024ULL * 1024ULL * 1024ULL;

    mz::MzMem mz_mem{};
    zlib_filefunc64_def file_func;
    if (!file_download) {
        mz::FileFuncMem(&mz_mem, &file_func);
    } else {
        mz::FileFuncStdio(&file_func);
    }

    {
        auto zfile = zipOpen2_64(temp_path, APPEND_STATUS_CREATE, nullptr, &file_func);
        R_UNLESS(zfile, 0x1);
        ON_SCOPE_EXIT(zipClose(zfile, "sphaira v" APP_VERSION_HASH));

        // add save meta.
        {
            const NXSaveMeta meta{
                .magic = NX_SAVE_META_MAGIC,
                .version = NX_SAVE_META_VERSION,
                .attr = extra.attr,
                .owner_id = extra.owner_id,
                .timestamp = extra.timestamp,
                .flags = extra.flags,
                .unk_x54 = extra.unk_x54,
                .data_size = extra.data_size,
                .journal_size = extra.journal_size,
                .commit_id = extra.commit_id,
                .raw_size = e.size,
            };

            R_UNLESS(ZIP_OK == zipOpenNewFileInZip(zfile, NX_SAVE_META_NAME, &zip_info_default, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_NO_COMPRESSION), 0x1);
            ON_SCOPE_EXIT(zipCloseFileInZip(zfile));
            R_UNLESS(ZIP_OK == zipWriteInFileInZip(zfile, &meta, sizeof(meta)), 0x1);
        }

        const auto zip_add = [&](const FsDirectoryEntry& dir_entry, const fs::FsPath& file_path) -> Result {
            auto zip_info = zip_info_default;

            // try and load the actual timestamp of the file.
            // TODO: not supported for saves...
            #if 0
            FsTimeStampRaw timestamp{};
            if (R_SUCCEEDED(fs.GetFileTimeStampRaw(file_path, &timestamp)) && timestamp.is_valid) {
                const auto time = (time_t)timestamp.modified;
                if (auto tm = localtime(&time)) {
                    zip_info.tmz_date.tm_sec = tm->tm_sec;
                    zip_info.tmz_date.tm_min = tm->tm_min;
                    zip_info.tmz_date.tm_hour = tm->tm_hour;
                    zip_info.tmz_date.tm_mday = tm->tm_mday;
                    zip_info.tmz_date.tm_mon = tm->tm_mon;
                    zip_info.tmz_date.tm_year = tm->tm_year;
                    log_write("got timestamp!\n");
                }
            }
            #endif

            // the file name needs to be relative to the current directory.
            const char* file_name_in_zip = file_path.s + std::strlen("/");

            // strip root path (/ or ums0:)
            if (!std::strncmp(file_name_in_zip, save_fs.Root(), std::strlen(save_fs.Root()))) {
                file_name_in_zip += std::strlen(save_fs.Root());
            }

            // root paths are banned in zips, they will warn when extracting otherwise.
            if (file_name_in_zip[0] == '/') {
                file_name_in_zip++;
            }

            pbox->NewTransfer(file_name_in_zip);

            const auto level = compressed ? Z_DEFAULT_COMPRESSION : Z_NO_COMPRESSION;
            if (ZIP_OK != zipOpenNewFileInZip(zfile, file_name_in_zip, &zip_info, NULL, 0, NULL, 0, NULL, Z_DEFLATED, level)) {
                log_write("failed to add zip for %s\n", file_path.s);
                R_THROW(0x1);
            }
            ON_SCOPE_EXIT(zipCloseFileInZip(zfile));

            return thread::TransferZip(pbox, zfile, &save_fs, file_path);
        };

        // loop through every save file and store to zip.
        for (const auto& collection : collections) {
            for (const auto& file : collection.files) {
                const auto file_path = fs::AppendPath(collection.path, file.name);
                R_TRY(zip_add(file, file_path));
            }
        }
    }

    // if we dumped the save to ram, flush the data to file.
    const auto is_file_based_emummc = App::IsFileBaseEmummc();
    if (!file_download) {
        pbox->NewTransfer("Flushing zip to file");
        R_TRY(fs.CreateFile(temp_path, mz_mem.buf.size(), 0));

        fs::File file;
        R_TRY(fs.OpenFile(temp_path, FsOpenMode_Write, &file));

        R_TRY(thread::Transfer(pbox, mz_mem.buf.size(),
            [&](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
                size = std::min<s64>(size, mz_mem.buf.size() - off);
                std::memcpy(data, mz_mem.buf.data() + off, size);
                *bytes_read = size;
                R_SUCCEED();
            },
            [&](const void* data, s64 off, s64 size) -> Result {
                const auto rc = file.Write(off, data, size, FsWriteOption_None);
                if (is_file_based_emummc) {
                    svcSleepThread(2e+6); // 2ms
                }
                return rc;
            }
        ));
    }

    fs.DeleteFile(path);
    R_TRY(fs.RenameFile(temp_path, path));

    App::Notify("Backed up to "_i18n + path.toString());
    R_SUCCEED();
}

void FreeEntry(NVGcontext* vg, Entry& e) {
    nvgDeleteImage(vg, e.image);
    e.image = 0;
}

void ThreadFunc(void* user) {
    auto data = static_cast<ThreadData*>(user);

    if (!nxtcInitialize()) {
        log_write("[NXTC] failed to init cache\n");
    }
    ON_SCOPE_EXIT(nxtcExit());

    while (data->IsRunning()) {
        data->Run();
    }
}

} // namespace

ThreadData::ThreadData() {
    ueventCreate(&m_uevent, true);
    mutexInit(&m_mutex_id);
    mutexInit(&m_mutex_result);
    m_running = true;
}

auto ThreadData::IsRunning() const -> bool {
    return m_running;
}

void ThreadData::Run() {
    const auto waiter = waiterForUEvent(&m_uevent);
    while (IsRunning()) {
        const auto rc = waitSingle(waiter, 3e+9);

        // if we timed out, flush the cache and poll again.
        if (R_FAILED(rc)) {
            nxtcFlushCacheFile();
            continue;
        }

        if (!IsRunning()) {
            return;
        }

        std::vector<u64> ids;
        {
            mutexLock(&m_mutex_id);
            ON_SCOPE_EXIT(mutexUnlock(&m_mutex_id));
            std::swap(ids, m_ids);
        }

        for (u64 i = 0; i < std::size(ids); i++) {
            if (!IsRunning()) {
                return;
            }

            ThreadResultData result{ids[i]};
            TimeStamp ts;
            if (auto data = nxtcGetApplicationMetadataEntryById(ids[i])) {
                log_write("[NXTC] loaded from cache time taken: %.2fs %zums %zuns\n", ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
                ON_SCOPE_EXIT(nxtcFreeApplicationMetadata(&data));

                result.control = std::make_unique<NsApplicationControlData>();
                result.status = NacpLoadStatus::Loaded;
                std::strcpy(result.lang.name, data->name);
                std::strcpy(result.lang.author, data->publisher);
                std::memcpy(result.control->icon, data->icon_data, data->icon_size);
                result.jpeg_size = data->icon_size;
            } else {
                // sleep after every other entry loaded.
                svcSleepThread(2e+6); // 2ms

                result = LoadControlEntry(ids[i]);
                if (result.status == NacpLoadStatus::Loaded) {
                    nxtcAddEntry(ids[i], &result.control->nacp, result.jpeg_size, result.control->icon, true);
                }
            }

            mutexLock(&m_mutex_result);
            ON_SCOPE_EXIT(mutexUnlock(&m_mutex_result));
            m_result.emplace_back(result);
        }
    }
}

void ThreadData::Close() {
    m_running = false;
    ueventSignal(&m_uevent);
}

void ThreadData::Push(u64 id) {
    mutexLock(&m_mutex_id);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex_id));

    const auto it = std::ranges::find(m_ids, id);
    if (it == m_ids.end()) {
        m_ids.emplace_back(id);
        ueventSignal(&m_uevent);
    }
}

void ThreadData::Push(std::span<const Entry> entries) {
    for (auto& e : entries) {
        Push(e.application_id);
    }
}

void ThreadData::Pop(std::vector<ThreadResultData>& out) {
    mutexLock(&m_mutex_result);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex_result));

    std::swap(out, m_result);
    m_result.clear();
}

Menu::Menu(u32 flags) : grid::Menu{"Saves"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::L3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            m_entries[m_index].selected ^= 1;

            if (m_entries[m_index].selected) {
                m_selected_count++;
            } else {
                m_selected_count--;
            }
        }}),
        std::make_pair(Button::R3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            if (m_selected_count == m_entries.size()) {
                ClearSelection();
            } else {
                m_selected_count = m_entries.size();
                for (auto& e : m_entries) {
                    e.selected = true;
                }
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_shared<Sidebar>("Save Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(options));

            SidebarEntryArray::Items account_items;
            for (const auto& e : m_accounts) {
                account_items.emplace_back(e.base.nickname);
            }

            if (m_entries.size()) {
                options->Add(std::make_shared<SidebarEntryCallback>("Sort By"_i18n, [this](){
                    auto options = std::make_shared<Sidebar>("Sort Options"_i18n, Sidebar::Side::RIGHT);
                    ON_SCOPE_EXIT(App::Push(options));

                    SidebarEntryArray::Items sort_items;
                    sort_items.push_back("Updated"_i18n);

                    SidebarEntryArray::Items order_items;
                    order_items.push_back("Descending"_i18n);
                    order_items.push_back("Ascending"_i18n);

                    SidebarEntryArray::Items layout_items;
                    layout_items.push_back("List"_i18n);
                    layout_items.push_back("Icon"_i18n);
                    layout_items.push_back("Grid"_i18n);

                    options->Add(std::make_shared<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
                        m_sort.Set(index_out);
                        SortAndFindLastFile(false);
                    }, m_sort.Get()));

                    options->Add(std::make_shared<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
                        m_order.Set(index_out);
                        SortAndFindLastFile(false);
                    }, m_order.Get()));

                    options->Add(std::make_shared<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
                        m_layout.Set(index_out);
                        OnLayoutChange();
                    }, m_layout.Get()));
                }));

                options->Add(std::make_shared<SidebarEntryArray>("Account"_i18n, account_items, [this](s64& index_out){
                    m_account_index = index_out;
                    m_dirty = true;
                }, m_account_index));

                options->Add(std::make_shared<SidebarEntryCallback>("Backup"_i18n, [this](){
                    std::vector<std::reference_wrapper<Entry>> entries;
                    if (m_selected_count) {
                        for (auto& e : m_entries) {
                            if (e.selected) {
                                entries.emplace_back(e);
                            }
                        }
                    } else {
                        entries.emplace_back(m_entries[m_index]);
                    }

                    BackupSaves(entries);
                }, true));

                options->Add(std::make_shared<SidebarEntryCallback>("Restore"_i18n, [this](){
                    RestoreSave();
                }, true));
            }

            options->Add(std::make_shared<SidebarEntryCallback>("Advanced"_i18n, [this](){
                auto options = std::make_shared<Sidebar>("Advanced Options"_i18n, Sidebar::Side::RIGHT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryBool>("Auto backup on restore"_i18n, m_auto_backup_on_restore.Get(), [this](bool& v_out){
                    m_auto_backup_on_restore.Set(v_out);
                }));

                options->Add(std::make_shared<SidebarEntryBool>("Compress backup"_i18n, m_compress_save_backup.Get(), [this](bool& v_out){
                    m_compress_save_backup.Set(v_out);
                }));
            }));
        }})
    );

    OnLayoutChange();
    nsInitialize();

    AccountUid uids[8];
    s32 account_count;
    if (R_SUCCEEDED(accountListAllUsers(uids, std::size(uids), &account_count))) {
        for (s32 i = 0; i < account_count; i++) {
            AccountProfile profile;
            if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
                AccountProfileBase base;
                if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
                    m_accounts.emplace_back(uids[i], profile, base);
                } else {
                    accountProfileClose(&profile);
                }
            }
        }
    }

    // try and find the last / default account and set that.
    AccountUid uid{};
    if (R_FAILED(accountTrySelectUserWithoutInteraction(&uid, false))) {
        accountGetLastOpenedUser(&uid);
    }

    const auto it = std::ranges::find_if(m_accounts, [&uid](auto& e){
        return !std::memcmp(&uid, &e.uid, sizeof(uid));
    });

    if (it != m_accounts.end()) {
        m_account_index = std::distance(m_accounts.begin(), it);
    }

    for (auto& e : ncm_entries) {
        e.Open();
    }

    threadCreate(&m_thread, ThreadFunc, &m_thread_data, nullptr, 1024*32, THREAD_PRIO, THREAD_CORE);
    svcSetThreadCoreMask(m_thread.handle, THREAD_CORE, THREAD_AFFINITY_DEFAULT(THREAD_CORE));
    threadStart(&m_thread);
}

Menu::~Menu() {
    m_thread_data.Close();

    for (auto& e : m_accounts) {
        accountProfileClose(&e.profile);
    }

    for (auto& e : ncm_entries) {
        e.Close();
    }

    FreeEntries();
    nsExit();

    threadWaitForExit(&m_thread);
    threadClose(&m_thread);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (m_dirty) {
        App::Notify("Updating application record list");
        SortAndFindLastFile(true);
    }

    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect_Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_entries.empty()) {
        gfx::drawTextArgs(vg, GetX() + GetW() / 2.f, GetY() + GetH() / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    std::vector<ThreadResultData> data;
    m_thread_data.Pop(data);

    for (const auto& d : data) {
        const auto it = std::ranges::find_if(m_entries, [&d](auto& e) {
            return e.application_id == d.id;
        });

        if (it != m_entries.end()) {
            LoadResultIntoEntry(*it, d);
        }
    }

    m_list->Draw(vg, theme, m_entries.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        auto& e = m_entries[pos];

        if (e.status == NacpLoadStatus::None) {
            m_thread_data.Push(e.application_id);
            e.status = NacpLoadStatus::Progress;
        }

        // lazy load image
        if (image_load_count < image_load_max) {
            if (LoadControlImage(e)) {
                image_load_count++;
            }
        }

        const auto selected = pos == m_index;
        DrawEntry(vg, theme, m_layout.Get(), v, selected, e.image, e.GetName(), e.GetAuthor(), "");

        if (e.selected) {
            gfx::drawRect(vg, v, nvgRGBA(0, 0, 0, 180), 5);
            gfx::drawText(vg, x + w / 2, y + h / 2, 24.f, "\uE14B", nullptr, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
        }
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    if (m_entries.empty()) {
        ScanHomebrew();
    }
}

void Menu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    char title[0x40];
    std::snprintf(title, sizeof(title), "%s | %016lX", m_accounts[m_account_index].base.nickname, m_entries[m_index].application_id);
    SetTitleSubHeading(title);
    this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries.size()));
}

void Menu::ScanHomebrew() {
    constexpr auto ENTRY_CHUNK_COUNT = 1000;
    TimeStamp ts;

    FreeEntries();
    m_entries.reserve(ENTRY_CHUNK_COUNT);

    if (m_accounts.empty()) {
        return;
    }

    FsSaveDataFilter filter{};
    filter.attr.uid = m_accounts[m_account_index].uid;
    filter.filter_by_user_id = true;

    FsSaveDataInfoReader reader;
    fsOpenSaveDataInfoReaderWithFilter(&reader, FsSaveDataSpaceId_User, &filter);
    ON_SCOPE_EXIT(fsSaveDataInfoReaderClose(&reader));

    std::vector<FsSaveDataInfo> info_list(ENTRY_CHUNK_COUNT);
    while (true) {
        s64 record_count{};
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, info_list.data(), info_list.size(), &record_count))) {
            log_write("failed fsSaveDataInfoReaderRead()\n");
        }

        // finished parsing all entries.
        if (!record_count) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            m_entries.emplace_back(info_list[i]);
        }
    }

    m_is_reversed = false;
    m_dirty = false;
    log_write("games found: %zu time_taken: %.2f seconds %zu ms %zu ns\n", m_entries.size(), ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
    this->Sort();
    SetIndex(0);
    ClearSelection();
}

void Menu::Sort() {
    // const auto sort = m_sort.Get();
    const auto order = m_order.Get();

    if (order == OrderType_Ascending) {
        if (!m_is_reversed) {
            std::ranges::reverse(m_entries);
            m_is_reversed = true;
        }
    } else {
        if (m_is_reversed) {
            std::ranges::reverse(m_entries);
            m_is_reversed = false;
        }
    }
}

void Menu::SortAndFindLastFile(bool scan) {
    const auto app_id = m_entries[m_index].application_id;
    if (scan) {
        ScanHomebrew();
    } else {
        Sort();
    }
    SetIndex(0);

    s64 index = -1;
    for (u64 i = 0; i < m_entries.size(); i++) {
        if (app_id == m_entries[i].application_id) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        const auto row = m_list->GetRow();
        const auto page = m_list->GetPage();
        // guesstimate where the position is
        if (index >= page) {
            m_list->SetYoff((((index - page) + row) / row) * m_list->GetMaxY());
        } else {
            m_list->SetYoff(0);
        }
        SetIndex(index);
    }
}

void Menu::FreeEntries() {
    auto vg = App::GetVg();

    for (auto&p : m_entries) {
        FreeEntry(vg, p);
    }

    m_entries.clear();
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

void Menu::BackupSaves(std::vector<std::reference_wrapper<Entry>>& entries) {
    int image = 0;
    if (entries.size() == 1) {
        image = entries[0].get().image;
    }

    App::Push(std::make_shared<OptionBox>(
        "Are you sure you want to backup save(s)?"_i18n,
        "Back"_i18n, "Backup"_i18n, 0, [this, entries](auto op_index){
            if (op_index && *op_index) {
                App::Push(std::make_shared<ProgressBox>(0, "Backup"_i18n, "", [this, entries](auto pbox) -> Result {
                    for (auto& e : entries) {
                        // the entry may not have loaded yet.
                        LoadControlEntry(e);
                        R_TRY(BackupSaveInternal(pbox, m_accounts[m_account_index], e, m_compress_save_backup.Get()));
                    }
                    R_SUCCEED();
                }, [](Result rc){
                    App::PushErrorBox(rc, "Backup failed!"_i18n);

                    if (R_SUCCEEDED(rc)) {
                        App::Notify("Backup successfull!"_i18n);
                    }
                }));
            }
        }, image
    ));
}

void Menu::RestoreSave() {
    const auto save_path = BuildSaveBasePath(m_entries[m_index]);

    fs::FsNativeSd fs;
    filebrowser::FsDirCollection collection;
    filebrowser::FsView::get_collection(&fs, save_path, "", collection, true, false, false);

    // reverse as they will be sorted in oldest -> newest.
    std::ranges::reverse(collection.files);

    std::vector<fs::FsPath> paths;
    PopupList::Items items;
    for (const auto&p : collection.files) {
        const auto view = std::string_view{p.name};
        if (view.starts_with("BCAT") || !view.ends_with(".zip")) {
            continue;
        }

        items.emplace_back(p.name);
        paths.emplace_back(fs::AppendPath(collection.path, p.name));
    }

    if (paths.empty()) {
        App::Push(std::make_shared<ui::OptionBox>(
            "No saves found in "_i18n + save_path.toString(),
            "OK"_i18n
        ));
        return;
    }

    const auto title = "Restore save for: "_i18n + m_entries[m_index].GetName();
    App::Push(std::make_shared<PopupList>(
        title, items, [this, paths, items](auto op_index){
            if (!op_index) {
                return;
            }

            const auto file_name = items[*op_index];
            const auto file_path = paths[*op_index];

            App::Push(std::make_shared<OptionBox>(
                "Are you sure you want to restore "_i18n + file_name + "?",
                "Back"_i18n, "Restore"_i18n, 0, [this, file_path](auto op_index){
                    if (op_index && *op_index) {
                        App::Push(std::make_shared<ProgressBox>(0, "Restore"_i18n, "", [this, file_path](auto pbox) -> Result {
                            // the entry may not have loaded yet.
                            LoadControlEntry(m_entries[m_index]);

                            if (m_auto_backup_on_restore.Get()) {
                                pbox->SetActionName("Auto backup"_i18n);
                                R_TRY(BackupSaveInternal(pbox, m_accounts[m_account_index], m_entries[m_index], m_compress_save_backup.Get(), true));
                            }

                            pbox->SetActionName("Restore"_i18n);
                            return RestoreSaveInternal(pbox, m_entries[m_index], file_path);
                        }, [this](Result rc){
                            App::PushErrorBox(rc, "Restore failed!"_i18n);

                            if (R_SUCCEEDED(rc)) {
                                App::Notify("Restore successfull!"_i18n);
                            }
                        }));
                    }
                }, m_entries[m_index].image
            ));
        }
    ));
}

} // namespace sphaira::ui::menu::save
