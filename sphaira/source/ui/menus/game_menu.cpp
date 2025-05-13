#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "ui/menus/game_menu.hpp"
#include "ui/sidebar.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/nca.hpp"

#include <utility>
#include <cstring>
#include <algorithm>

namespace sphaira::ui::menu::game {
namespace {

constexpr NcmStorageId NCM_STORAGE_IDS[]{
    NcmStorageId_BuiltInUser,
    NcmStorageId_SdCard,
};

NcmContentStorage ncm_cs[2];
NcmContentMetaDatabase ncm_db[2];

auto& GetNcmCs(u8 storage_id) {
    if (storage_id == NcmStorageId_SdCard) {
        return ncm_cs[1];
    }
    return ncm_cs[0];
}

auto& GetNcmDb(u8 storage_id) {
    if (storage_id == NcmStorageId_SdCard) {
        return ncm_db[1];
    }
    return ncm_db[0];
}

using MetaEntries = std::vector<NsApplicationContentMetaStatus>;

Result Notify(Result rc, const std::string& error_message) {
    if (R_FAILED(rc)) {
        App::Push(std::make_shared<ui::ErrorBox>(rc,
            i18n::get(error_message)
        ));
    } else {
        App::Notify("Success");
    }

    return rc;
}

Result GetMetaEntries(u64 id, MetaEntries& out) {
    s32 count;
    R_TRY(nsCountApplicationContentMeta(id, &count));

    out.resize(count);
    R_TRY(nsListApplicationContentMetaStatus(id, 0, out.data(), out.size(), &count));

    out.resize(count);
    R_SUCCEED();
}

Result GetMetaEntries(const Entry& e, MetaEntries& out) {
    return GetMetaEntries(e.app_id, out);
}

// also sets the status to error.
void FakeNacpEntry(ThreadResultData& e) {
    e.status = NacpLoadStatus::Error;
    // fake the nacp entry
    std::strcpy(e.lang.name, "Corrupted");
    std::strcpy(e.lang.author, "Corrupted");
    std::strcpy(e.display_version, "0.0.0");
    e.control.reset();
}

bool LoadControlImage(Entry& e) {
    if (!e.image && e.control) {
        TimeStamp ts;
        const auto jpeg_size = e.control_size - sizeof(NacpStruct);
        e.image = nvgCreateImageMem(App::GetVg(), 0, e.control->icon, jpeg_size);
        e.control.reset();
        log_write("\t\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
        return true;
    }

    return false;
}

Result LoadControlManual(u64 id, ThreadResultData& data) {
    TimeStamp ts;

    MetaEntries entries;
    R_TRY(GetMetaEntries(id, entries));
    R_UNLESS(!entries.empty(), 0x1);

    const auto& ee = entries.back();
    if (ee.storageID != NcmStorageId_SdCard && ee.storageID != NcmStorageId_BuiltInUser) {
        return 0x1;
    }

    auto& db = GetNcmDb(ee.storageID);
    auto& cs = GetNcmCs(ee.storageID);

    NcmContentMetaKey key;
    R_TRY(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, ee.application_id));

    NcmContentId content_id;
    R_TRY(ncmContentMetaDatabaseGetContentIdByType(&db, &content_id, &key, NcmContentType_Control));

    u64 program_id;
    R_TRY(ncmContentStorageGetProgramId(&cs, &program_id, &content_id, FsContentAttributes_All));

    fs::FsPath path;
    R_TRY(ncmContentStorageGetPath(&cs, path, sizeof(path), &content_id));

    std::vector<u8> icon;
    R_TRY(nca::ParseControl(path, program_id, &data.control->nacp, sizeof(data.control->nacp), &icon));
    std::memcpy(data.control->icon, icon.data(), icon.size());

    data.control_size = sizeof(data.control->nacp) + icon.size();
    log_write("\t\t[manual control] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());

    R_SUCCEED();
}

auto LoadControlEntry(u64 id) -> ThreadResultData {
    ThreadResultData data{};
    data.id = id;
    data.control = std::make_shared<NsApplicationControlData>();
    data.status = NacpLoadStatus::Error;

    bool manual_load = false;
    if (hosversionBefore(20,0,0)) {
        TimeStamp ts;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, id, data.control.get(), sizeof(NsApplicationControlData), &data.control_size))) {
            manual_load = false;
            log_write("\t\t[ns control cache] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
        }
    }

    if (manual_load) {
        manual_load = R_SUCCEEDED(LoadControlManual(id, data));
    }

    Result rc{};
    if (!manual_load) {
        TimeStamp ts;
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, data.control.get(), sizeof(NsApplicationControlData), &data.control_size);
        log_write("\t\t[ns control storage] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
    }

    if (R_SUCCEEDED(rc)) {
        NacpLanguageEntry* lang{};
        if (R_SUCCEEDED(rc = nsGetApplicationDesiredLanguage(&data.control->nacp, &lang)) && lang) {
            data.lang = *lang;
            std::memcpy(data.display_version, data.control->nacp.display_version, sizeof(data.display_version));
            data.status = NacpLoadStatus::Loaded;
        }
    }

    if (R_FAILED(rc)) {
        FakeNacpEntry(data);
    }

    return data;
}

void LoadResultIntoEntry(Entry& e, const ThreadResultData& result) {
    e.status = result.status;
    e.control = result.control;
    e.control_size = result.control_size;
    std::memcpy(e.display_version, result.display_version, sizeof(result.display_version));
    e.lang = result.lang;
    e.status = result.status;
}

void LoadControlEntry(Entry& e, bool force_image_load = false) {
    if (e.status == NacpLoadStatus::None) {
        const auto result = LoadControlEntry(e.app_id);
        LoadResultIntoEntry(e, result);
    }

    if (force_image_load && e.status == NacpLoadStatus::Loaded) {
        LoadControlImage(e);
    }
}

void FreeEntry(NVGcontext* vg, Entry& e) {
    nvgDeleteImage(vg, e.image);
    e.image = 0;
}

void LaunchEntry(const Entry& e) {
    const auto rc = appletRequestLaunchApplication(e.app_id, nullptr);
    Notify(rc, "Failed to launch application");
}

void ThreadFunc(void* user) {
    auto data = static_cast<ThreadData*>(user);

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
    while (IsRunning()) {
        const auto waiter = waiterForUEvent(&m_uevent);
        waitSingle(waiter, UINT64_MAX);

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

            // sleep after every other entry loaded.
            if (i) {
                svcSleepThread(1e+6*2); // 2ms
            }

            const auto result = LoadControlEntry(ids[i]);
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

    const auto it = std::find(m_ids.begin(), m_ids.end(), id);
    if (it == m_ids.end()) {
        m_ids.emplace_back(id);
        ueventSignal(&m_uevent);
    }
}

void ThreadData::Push(std::span<const Entry> entries) {
    for (auto& e : entries) {
        Push(e.app_id);
    }
}

void ThreadData::Pop(std::vector<ThreadResultData>& out) {
    mutexLock(&m_mutex_result);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex_result));

    std::swap(out, m_result);
    m_result.clear();
}

Menu::Menu() : grid::Menu{"Games"_i18n} {
    this->SetActions(
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::A, Action{"Launch"_i18n, [this](){
            if (m_entries.empty()) {
                return;
            }
            LaunchEntry(m_entries[m_index]);
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_shared<Sidebar>("Game Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(options));

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

                    options->Add(std::make_shared<SidebarEntryBool>("Hide forwarders"_i18n, m_hide_forwarders.Get(), [this](bool& v_out){
                        m_hide_forwarders.Set(v_out);
                        m_dirty = true;
                    }));
                }));

                #if 0
                options->Add(std::make_shared<SidebarEntryCallback>("Info"_i18n, [this](){

                }));
                #endif

                options->Add(std::make_shared<SidebarEntryCallback>("Launch random game"_i18n, [this](){
                    const auto random_index = randomGet64() % std::size(m_entries);
                    auto& e = m_entries[random_index];
                    LoadControlEntry(e, true);

                    App::Push(std::make_shared<OptionBox>(
                        "Launch "_i18n + e.GetName(),
                        "Back"_i18n, "Launch"_i18n, 1, [this, &e](auto op_index){
                            if (op_index && *op_index) {
                                LaunchEntry(e);
                            }
                        }, e.image
                    ));
                }));

                options->Add(std::make_shared<SidebarEntryCallback>("List meta records"_i18n, [this](){
                    MetaEntries meta_entries;
                    const auto rc = GetMetaEntries(m_entries[m_index], meta_entries);
                    if (R_FAILED(rc)) {
                        App::Push(std::make_shared<ui::ErrorBox>(rc,
                            i18n::get("Failed to list application meta entries")
                        ));
                        return;
                    }

                    if (meta_entries.empty()) {
                        App::Notify("No meta entries found...\n");
                        return;
                    }

                    PopupList::Items items;
                    for (auto& e : meta_entries) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "Type: %s Storage: %s [%016lX][v%u]", ncm::GetMetaTypeStr(e.meta_type), ncm::GetStorageIdStr(e.storageID), e.application_id, e.version);
                        items.emplace_back(buf);
                    }

                    App::Push(std::make_shared<PopupList>(
                        "Entries", items, [this, meta_entries](auto op_index){
                            #if 0
                            if (op_index) {
                                const auto& e = meta_entries[*op_index];
                            }
                            #endif
                        }
                    ));
                }));

                // completely deletes the application record and all data.
                options->Add(std::make_shared<SidebarEntryCallback>("Delete"_i18n, [this](){
                    const auto buf = "Are you sure you want to delete "_i18n + m_entries[m_index].GetName() + "?";
                    App::Push(std::make_shared<OptionBox>(
                        buf,
                        "Back"_i18n, "Delete"_i18n, 0, [this](auto op_index){
                            if (op_index && *op_index) {
                                const auto rc = nsDeleteApplicationCompletely(m_entries[m_index].app_id);
                                Notify(rc, "Failed to delete application");
                            }
                        }, m_entries[m_index].image
                    ));
                }, true));

                // removes installed data but keeps the record, basically archiving.
                options->Add(std::make_shared<SidebarEntryCallback>("Delete entity"_i18n, [this](){
                    const auto buf = "Are you sure you want to delete "_i18n + m_entries[m_index].GetName() + "?";
                    App::Push(std::make_shared<OptionBox>(
                        buf,
                        "Back"_i18n, "Delete"_i18n, 0, [this](auto op_index){
                            if (op_index && *op_index) {
                                const auto rc = nsDeleteApplicationEntity(m_entries[m_index].app_id);
                                Notify(rc, "Failed to delete application");
                            }
                        }, m_entries[m_index].image
                    ));
                }, true));
            }
        }})
    );

    OnLayoutChange();

    nsInitialize();
    nsGetApplicationRecordUpdateSystemEvent(&m_event);

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        ncmOpenContentMetaDatabase(std::addressof(ncm_db[i]), NCM_STORAGE_IDS[i]);
        ncmOpenContentStorage(std::addressof(ncm_cs[i]), NCM_STORAGE_IDS[i]);
    }

    threadCreate(&m_thread, ThreadFunc, &m_thread_data, nullptr, 1024*32, 0x30, 1);
    threadStart(&m_thread);
}

Menu::~Menu() {
    m_thread_data.Close();

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        ncmContentMetaDatabaseClose(std::addressof(ncm_db[i]));
        ncmContentStorageClose(std::addressof(ncm_cs[i]));
    }

    FreeEntries();
    eventClose(&m_event);
    nsExit();

    threadWaitForExit(&m_thread);
    threadClose(&m_thread);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (R_SUCCEEDED(eventWait(&m_event, 0))) {
        log_write("got ns event\n");
        m_dirty = true;
    }

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

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    std::vector<ThreadResultData> data;
    m_thread_data.Pop(data);

    for (const auto& d : data) {
        const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&d](auto& e) {
            return e.app_id == d.id;
        });

        if (it != m_entries.end()) {
            LoadResultIntoEntry(*it, d);
        }
    }

    m_list->Draw(vg, theme, m_entries.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        // const auto& [x, y, w, h] = v;
        auto& e = m_entries[pos];

        if (e.status == NacpLoadStatus::None) {
            m_thread_data.Push(e.app_id);
            e.status = NacpLoadStatus::Progress;
        }

        // lazy load image
        if (image_load_count < image_load_max) {
            if (LoadControlImage(e)) {
                image_load_count++;
            }
        }

        const auto selected = pos == m_index;
        DrawEntry(vg, theme, m_layout.Get(), v, selected, e.image, e.GetName(), e.GetAuthor(), e.GetDisplayVersion());
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

    char title_id[33];
    std::snprintf(title_id, sizeof(title_id), "%016lX", m_entries[m_index].app_id);
    SetTitleSubHeading(title_id);
    this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries.size()));
}

void Menu::ScanHomebrew() {
    constexpr auto ENTRY_CHUNK_COUNT = 1000;
    const auto hide_forwarders = m_hide_forwarders.Get();
    TimeStamp ts;

    FreeEntries();
    m_entries.reserve(ENTRY_CHUNK_COUNT);

    // wait on event in order to clear it as this event will trigger when
    // an application is launched, causing a double list.
    eventWait(&m_event, 0);

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset{};
    while (true) {
        s32 record_count{};
        if (R_FAILED(nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count))) {
            log_write("failed to list application records at offset: %d\n", offset);
        }

        // finished parsing all entries.
        if (!record_count) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            const auto& e = record_list[i];
            #if 0
            u8 unk_x09 = e.unk_x09;
            u64 unk_x0a;// = e.unk_x0a;
            u8 unk_x10 = e.unk_x10;
            u64 unk_x11;// = e.unk_x11;
            memcpy(&unk_x0a, e.unk_x0a, sizeof(e.unk_x0a));
            memcpy(&unk_x11, e.unk_x11, sizeof(e.unk_x11));
            log_write("ID: %016lx got type: %u unk_x09: %u unk_x0a: %zu unk_x10: %u unk_x11: %zu\n", e.application_id, e.type,
                unk_x09,
                unk_x0a,
                unk_x10,
                unk_x11
            );
            #endif
            if (hide_forwarders && (e.application_id & 0x0500000000000000) == 0x0500000000000000) {
                continue;
            }

            m_entries.emplace_back(e.application_id);
        }

        offset += record_count;
    }

    m_is_reversed = false;
    m_dirty = false;
    log_write("games found: %zu time_taken: %.2f seconds %zu ms %zu ns\n", m_entries.size(), ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
    this->Sort();
    SetIndex(0);

    // m_thread_data.Push(m_entries);
}

void Menu::Sort() {
    // const auto sort = m_sort.Get();
    const auto order = m_order.Get();

    if (order == OrderType_Ascending) {
        if (!m_is_reversed) {
            std::reverse(m_entries.begin(), m_entries.end());
            m_is_reversed = true;
        }
    } else {
        if (m_is_reversed) {
            std::reverse(m_entries.begin(), m_entries.end());
            m_is_reversed = false;
        }
    }
}

void Menu::SortAndFindLastFile(bool scan) {
    const auto app_id = m_entries[m_index].app_id;
    if (scan) {
        ScanHomebrew();
    } else {
        Sort();
    }
    SetIndex(0);

    s64 index = -1;
    for (u64 i = 0; i < m_entries.size(); i++) {
        if (app_id == m_entries[i].app_id) {
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

} // namespace sphaira::ui::menu::game
