#pragma once

#include "ui/menus/menu_base.hpp"
#include "yati/container/base.hpp"
#include "yati/source/base.hpp"
#include "ui/list.hpp"
#include <span>
#include <memory>

namespace sphaira::ui::menu::gc {

typedef enum {
    FsGameCardStoragePartition_None   = -1,
    FsGameCardStoragePartition_Normal = 0,
    FsGameCardStoragePartition_Secure = 1,
} FsGameCardStoragePartition;

struct GcCollection : yati::container::CollectionEntry {
    GcCollection(const char* _name, s64 _size, u8 _type, u8 _id_offset) {
        name = _name;
        size = _size;
        type = _type;
        id_offset = _id_offset;
    }

    // NcmContentType
    u8 type{};
    u8 id_offset{};
};

using GcCollections = std::vector<GcCollection>;

struct ApplicationEntry {
    u64 app_id{};
    u32 version{};
    u8 key_gen{};
    std::unique_ptr<NsApplicationControlData> control{};
    u64 control_size{};
    NacpLanguageEntry lang_entry{};

    std::vector<GcCollections> application{};
    std::vector<GcCollections> patch{};
    std::vector<GcCollections> add_on{};
    std::vector<GcCollections> data_patch{};
    yati::container::Collections tickets{};

    auto GetSize() const -> s64;
    auto GetSize(const std::vector<GcCollections>& entries) const -> s64;
};

struct Menu final : MenuBase {
    Menu();
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "GC"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

    Result GcStorageRead(void* buf, s64 off, s64 size);

private:
    Result GcPoll(bool* inserted);
    Result GcOnEvent();

    // GameCard FS api.
    Result GcMount();
    void GcUnmount();

    // GameCard Storage api
    Result GcMountStorage();
    void GcUmountStorage();
    Result GcMountPartition(FsGameCardStoragePartition partition);
    void GcUnmountPartition();
    Result GcStorageReadInternal(void* buf, s64 off, s64 size, u64* bytes_read);

    Result LoadControlData(ApplicationEntry& e);
    Result UpdateStorageSize();
    void FreeImage();
    void OnChangeIndex(s64 new_index);
    void DumpGames(u32 flags);

private:
    FsDeviceOperator m_dev_op{};
    FsGameCardHandle m_handle{};
    std::unique_ptr<fs::FsNativeGameCard> m_fs{};
    FsEventNotifier m_event_notifier{};
    Event m_event{};

    std::vector<ApplicationEntry> m_entries{};
    std::unique_ptr<List> m_list{};
    s64 m_entry_index{};
    s64 m_option_index{};

    s64 m_size_free_sd{};
    s64 m_size_total_sd{};
    s64 m_size_free_nand{};
    s64 m_size_total_nand{};
    int m_icon{};
    bool m_mounted{};

    FsStorage m_storage{};
    s64 m_parition_normal_size{};
    s64 m_parition_secure_size{};
    s64 m_storage_trimmed_size{};
    s64 m_storage_total_size{};
    u8 m_initial_data_hash[SHA256_HASH_SIZE]{};
    FsGameCardStoragePartition m_partition{FsGameCardStoragePartition_None};
    bool m_storage_mounted{};
};

} // namespace sphaira::ui::menu::gc
