#include "ui/menus/themezer.hpp"
#include "ui/progress_box.hpp"
#include "ui/option_box.hpp"
#include "ui/sidebar.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "download.hpp"
#include "ui/nvg_util.hpp"
#include "swkbd.hpp"
#include "i18n.hpp"

#include <minIni.h>
#include <stb_image.h>
#include <cstring>
#include <minizip/unzip.h>
#include <yyjson.h>
#include "yyjson_helper.hpp"

namespace sphaira::ui::menu::themezer {
namespace {

// format is /themes/sphaira/Theme Name by Author/theme_name-type.nxtheme
constexpr fs::FsPath THEME_FOLDER{"/themes/sphaira/"};
constexpr auto CACHE_PATH = "/switch/sphaira/cache/themezer";
constexpr auto URL_BASE = "https://switch.cdn.fortheusers.org";

constexpr const char* REQUEST_TARGET[]{
    "ResidentMenu",
    "Entrance",
    "Flaunch",
    "Set",
    "Psl",
    "MyPage",
    "Notification"
};

constexpr const char* REQUEST_SORT[]{
    "downloads",
    "updated",
    "likes",
    "id"
};

constexpr const char* REQUEST_ORDER[]{
    "desc",
    "asc"
};

// https://api.themezer.net/?query=query($nsfw:Boolean,$target:String,$page:Int,$limit:Int,$sort:String,$order:String,$query:String,$creators:[String!]){themeList(nsfw:$nsfw,target:$target,page:$page,limit:$limit,sort:$sort,order:$order,query:$query,creators:$creators){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}&variables={"nsfw":false,"target":null,"page":1,"limit":10,"sort":"updated","order":"desc","query":null,"creators":["695065006068334622"]}
// https://api.themezer.net/?query=query($nsfw:Boolean,$page:Int,$limit:Int,$sort:String,$order:String,$query:String,$creators:[String!]){packList(nsfw:$nsfw,page:$page,limit:$limit,sort:$sort,order:$order,query:$query,creators:$creators){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,themes{id,creator{display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}}&variables={"nsfw":false,"page":1,"limit":10,"sort":"updated","order":"desc","query":null,"creators":["695065006068334622"]}

// i know, this is cursed
// todo: send actual POST request rather than GET.
auto apiBuildUrlListInternal(const Config& e, bool is_pack) -> std::string {
    std::string api = "https://api.themezer.net/?query=query";
    // std::string fields = "{id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count";
    std::string fields = "{id,creator{id,display_name},details{name}";
    const char* boolarr[2] = { "false", "true" };

    std::string cmd;
    std::string p0 = "$nsfw:Boolean,$page:Int,$limit:Int,$sort:String,$order:String";
    std::string p1 = "nsfw:$nsfw,page:$page,limit:$limit,sort:$sort,order:$order";
    std::string json = "\"nsfw\":"+std::string{boolarr[e.nsfw]}+",\"page\":"+std::to_string(e.page)+",\"limit\":"+std::to_string(e.limit)+",\"sort\":\""+std::string{REQUEST_SORT[e.sort_index]}+"\",\"order\":\""+std::string{REQUEST_ORDER[e.order_index]}+"\"";

    if (is_pack) {
        cmd = "packList";
        // fields += ",themes{id,creator{display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}";
        fields += ",themes{id,preview{thumb}}";
    } else {
        cmd = "themeList";
        p0 += ",$target:String";
        p1 += ",target:$target";
        if (e.target_index < 7) {
            json += ",\"target\":\"" + std::string{REQUEST_TARGET[e.target_index]} + "\"";
        } else {
            json += ",\"target\":null";
        }
    }

    if (!e.creator.empty()) {
        p0 += ",$creators:[String!]";
        p1 += ",creators:$creators";
        json += ",\"creators\":[\"" + e.creator + "\"]";
    }

    if (!e.query.empty()) {
        p0 += ",$query:String";
        p1 += ",query:$query";
        json += ",\"query\":\"" + e.query + "\"";
    }

    json = curl::EscapeString('{'+json+'}');

    return api+"("+p0+"){"+cmd+"("+p1+")"+fields+"}}&variables="+json;
}

auto apiBuildUrlDownloadInternal(const std::string& id, bool is_pack) -> std::string {
    char url[2048];
    std::snprintf(url, sizeof(url), "https://api.themezer.net/?query=query{download%s(id:\"%s\"){filename,url,mimetype}}", is_pack ? "Pack" : "Theme", id.c_str());
    return url;
    // https://api.themezer.net/?query=query{downloadPack(id:"11"){filename,url,mimetype}}
}

auto apiBuildUrlDownloadPack(const PackListEntry& e) -> std::string {
    return apiBuildUrlDownloadInternal(e.id, true);
}

auto apiBuildUrlListPacks(const Config& e) -> std::string {
    return apiBuildUrlListInternal(e, true);
}

auto apiBuildListPacksCache(const Config& e) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%u_page.json", CACHE_PATH, e.page);
    return path;
}

auto apiBuildIconCache(const ThemeEntry& e) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s_thumb.jpg", CACHE_PATH, e.id.c_str());
    return path;
}

auto loadThemeImage(ThemeEntry& e) -> bool {
    auto& image = e.preview.lazy_image;

    // already have the image
    if (e.preview.lazy_image.image) {
        // log_write("warning, tried to load image: %s when already loaded\n", path.c_str());
        return true;
    }
    auto vg = App::GetVg();

    fs::FsNativeSd fs;
    std::vector<u8> image_buf;

    const auto path = apiBuildIconCache(e);
    if (R_FAILED(fs.read_entire_file(path, image_buf))) {
        log_write("failed to load image from file: %s\n", path.s);
    } else {
        int channels_in_file;
        auto buf = stbi_load_from_memory(image_buf.data(), image_buf.size(), &image.w, &image.h, &channels_in_file, 4);
        if (buf) {
            ON_SCOPE_EXIT(stbi_image_free(buf));
            image.image = nvgCreateImageRGBA(vg, image.w, image.h, 0, buf);
        }
    }

    if (!image.image) {
        log_write("failed to load image from file: %s\n", path.s);
        return false;
    } else {
        // log_write("loaded image from file: %s\n", path);
        return true;
    }
}

void from_json(yyjson_val* json, Creator& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(id);
        JSON_SET_STR(display_name);
    );
}

void from_json(yyjson_val* json, Details& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(name);
    );
}

void from_json(yyjson_val* json, Preview& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(thumb);
    );
}

void from_json(yyjson_val* json, ThemeEntry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(id);
        JSON_SET_OBJ(preview);
    );
}

void from_json(yyjson_val* json, PackListEntry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(id);
        JSON_SET_OBJ(creator);
        JSON_SET_OBJ(details);
        JSON_SET_ARR_OBJ(themes);
    );
}

void from_json(yyjson_val* json, Pagination& e) {
    JSON_OBJ_ITR(
        JSON_SET_UINT(page);
        JSON_SET_UINT(limit);
        JSON_SET_UINT(page_count);
        JSON_SET_UINT(item_count);
    );
}

void from_json(const std::vector<u8>& data, DownloadPack& e) {
    JSON_INIT_VEC(data, "data");
    JSON_GET_OBJ("downloadPack");
    JSON_OBJ_ITR(
        JSON_SET_STR(filename);
        JSON_SET_STR(url);
        JSON_SET_STR(mimetype);
    );
}

void from_json(const fs::FsPath& path, PackList& e) {
    JSON_INIT_VEC_FILE(path, "data", nullptr);
    JSON_OBJ_ITR(
        JSON_SET_ARR_OBJ(packList);
        JSON_SET_OBJ(pagination);
    );
}

auto InstallTheme(ProgressBox* pbox, const PackListEntry& entry) -> bool {
    static const fs::FsPath zip_out{"/switch/sphaira/cache/themezer/temp.zip"};
    constexpr auto chunk_size = 1024 * 512; // 512KiB

    fs::FsNativeSd fs;
    R_TRY_RESULT(fs.GetFsOpenResult(), false);

    DownloadPack download_pack;

    // 1. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading "_i18n + entry.details.name);
        log_write("starting download\n");

        const auto url = apiBuildUrlDownloadPack(entry);
        log_write("using url: %s\n", url.c_str());
        const auto result = curl::Api().ToMemory(
            curl::Url{url},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        if (!result.success || result.data.empty()) {
            log_write("error with download: %s\n", url.c_str());
            return false;
        }

        from_json(result.data, download_pack);
    }

    // 2. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading "_i18n + entry.details.name);
        log_write("starting download: %s\n", download_pack.url.c_str());

        if (!curl::Api().ToFile(
            curl::Url{download_pack.url},
            curl::Path{zip_out},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}).success) {
            log_write("error with download\n");
            return false;
        }
    }

    ON_SCOPE_EXIT(fs.DeleteFile(zip_out));

    // create directories
    fs::FsPath dir_path;
    std::snprintf(dir_path, sizeof(dir_path), "%s/%s - By %s", THEME_FOLDER.s, entry.details.name.c_str(), entry.creator.display_name.c_str());
    fs.CreateDirectoryRecursively(dir_path);

    // 3. extract the zip
    if (!pbox->ShouldExit()) {
        auto zfile = unzOpen64(zip_out);
        if (!zfile) {
            log_write("failed to open zip: %s\n", zip_out.s);
            return false;
        }
        ON_SCOPE_EXIT(unzClose(zfile));

        unz_global_info64 pglobal_info;
        if (UNZ_OK != unzGetGlobalInfo64(zfile, &pglobal_info)) {
            return false;
        }

        for (int i = 0; i < pglobal_info.number_entry; i++) {
            if (i > 0) {
                if (UNZ_OK != unzGoToNextFile(zfile)) {
                    log_write("failed to unzGoToNextFile\n");
                    return false;
                }
            }

            if (UNZ_OK != unzOpenCurrentFile(zfile)) {
                log_write("failed to open current file\n");
                return false;
            }
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            char name[512];
            if (UNZ_OK != unzGetCurrentFileInfo64(zfile, &info, name, sizeof(name), 0, 0, 0, 0)) {
                log_write("failed to get current info\n");
                return false;
            }

            const auto file_path = fs::AppendPath(dir_path, name);

            Result rc;
            if (R_FAILED(rc = fs.CreateFile(file_path, info.uncompressed_size, 0)) && rc != FsError_PathAlreadyExists) {
                log_write("failed to create file: %s 0x%04X\n", file_path.s, rc);
                return false;
            }

            FsFile f;
            if (R_FAILED(rc = fs.OpenFile(file_path, FsOpenMode_Write, &f))) {
                log_write("failed to open file: %s 0x%04X\n", file_path.s, rc);
                return false;
            }
            ON_SCOPE_EXIT(fsFileClose(&f));

            if (R_FAILED(rc = fsFileSetSize(&f, info.uncompressed_size))) {
                log_write("failed to set file size: %s 0x%04X\n", file_path.s, rc);
                return false;
            }

            std::vector<char> buf(chunk_size);
            s64 offset{};
            while (offset < info.uncompressed_size) {
                if (pbox->ShouldExit()) {
                    return false;
                }

                const auto bytes_read = unzReadCurrentFile(zfile, buf.data(), buf.size());
                if (bytes_read <= 0) {
                    // log_write("failed to read zip file: %s\n", inzip.c_str());
                    return false;
                }

                if (R_FAILED(rc = fsFileWrite(&f, offset, buf.data(), bytes_read, FsWriteOption_None))) {
                    log_write("failed to write file: %s 0x%04X\n", file_path.s, rc);
                    return false;
                }

                pbox->UpdateTransfer(offset, info.uncompressed_size);
                offset += bytes_read;
            }
        }
    }

    log_write("finished install :)\n");
    return true;
}

} // namespace

LazyImage::~LazyImage() {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

Menu::Menu() : MenuBase{"Themezer"_i18n} {
    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);

    SetAction(Button::B, Action{"Back"_i18n, [this]{
        // if search is valid, then we are in search mode, return back to normal.
        if (!m_search.empty()) {
            m_search.clear();
            InvalidateAllPages();
        } else {
            SetPop();
        }
    }});

    this->SetActions(
        std::make_pair(Button::RIGHT, Action{[this](){
            const auto& page = m_pages[m_page_index];
            if (m_index < (page.m_packList.size() - 1) && (m_index + 1) % 3 != 0) {
                SetIndex(m_index + 1);
                App::PlaySoundEffect(SoundEffect_Scroll);
                log_write("moved right\n");
            }
        }}),
        std::make_pair(Button::LEFT, Action{[this](){
            if (m_index != 0 && (m_index % 3) != 0) {
                SetIndex(m_index - 1);
                App::PlaySoundEffect(SoundEffect_Scroll);
                log_write("moved left\n");
            }
        }}),
        std::make_pair(Button::DOWN, Action{[this](){
            const auto& page = m_pages[m_page_index];
            if (m_list->ScrollDown(m_index, 3, page.m_packList.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::UP, Action{[this](){
            const auto& page = m_pages[m_page_index];
            if (m_list->ScrollUp(m_index, 3, page.m_packList.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::R2, Action{[this](){
            const auto& page = m_pages[m_page_index];
            if (m_list->ScrollDown(m_index, 6, page.m_packList.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::L2, Action{[this](){
            const auto& page = m_pages[m_page_index];
            if (m_list->ScrollUp(m_index, 6, page.m_packList.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::A, Action{"Download"_i18n, [this](){
            App::Push(std::make_shared<OptionBox>(
                "Download theme?"_i18n,
                "Back"_i18n, "Download"_i18n, 1, [this](auto op_index){
                    if (op_index && *op_index) {
                        const auto& page = m_pages[m_page_index];
                        if (page.m_packList.size() && page.m_ready == PageLoadState::Done) {
                            const auto& entry = page.m_packList[m_index];
                            const auto url = apiBuildUrlDownloadPack(entry);

                            App::Push(std::make_shared<ProgressBox>("Installing "_i18n + entry.details.name, [this, &entry](auto pbox){
                                return InstallTheme(pbox, entry);
                            }, [this, &entry](bool success){
                                if (success) {
                                    App::Notify("Downloaded "_i18n + entry.details.name);
                                }
                            }, 2));
                        }
                    }
                }
            ));
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_shared<Sidebar>("Themezer Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(options));

            SidebarEntryArray::Items sort_items;
            sort_items.push_back("Downloads"_i18n);
            sort_items.push_back("Updated"_i18n);
            sort_items.push_back("Likes"_i18n);
            sort_items.push_back("ID"_i18n);

            SidebarEntryArray::Items order_items;
            order_items.push_back("Descending (down)"_i18n);
            order_items.push_back("Ascending (Up)"_i18n);

            options->Add(std::make_shared<SidebarEntryBool>("Nsfw"_i18n, m_nsfw.Get(), [this](bool& v_out){
                m_nsfw.Set(v_out);
                InvalidateAllPages();
            }, "Enabled"_i18n, "Disabled"_i18n));

            options->Add(std::make_shared<SidebarEntryArray>("Sort"_i18n, sort_items, [this, sort_items](s64& index_out){
                if (m_sort.Get() != index_out) {
                    m_sort.Set(index_out);
                    InvalidateAllPages();
                }
            }, m_sort.Get()));

            options->Add(std::make_shared<SidebarEntryArray>("Order"_i18n, order_items, [this, order_items](s64& index_out){
                if (m_order.Get() != index_out) {
                    m_order.Set(index_out);
                    InvalidateAllPages();
                }
            }, m_order.Get()));

            options->Add(std::make_shared<SidebarEntryCallback>("Page"_i18n, [this](){
                s64 out;
                if (R_SUCCEEDED(swkbd::ShowNumPad(out, "Enter Page Number"_i18n.c_str(), nullptr, -1, 3))) {
                    if (out < m_page_index_max) {
                        m_page_index = out;
                        PackListDownload();
                    } else {
                        log_write("invalid page number\n");
                        App::Notify("Bad Page"_i18n);
                    }
                }
            }));

            options->Add(std::make_shared<SidebarEntryCallback>("Search"_i18n, [this](){
                std::string out;
                if (R_SUCCEEDED(swkbd::ShowText(out)) && !out.empty()) {
                    m_search = out;
                    // PackListDownload();
                    InvalidateAllPages();
                }
            }));
        }}),
        std::make_pair(Button::R, Action{"Next Page"_i18n, [this](){
            m_page_index++;
            if (m_page_index >= m_page_index_max) {
                m_page_index = m_page_index_max - 1;
            } else {
                PackListDownload();
            }
        }}),
        std::make_pair(Button::L, Action{"Prev Page"_i18n, [this](){
            if (m_page_index) {
                m_page_index--;
                PackListDownload();
            }
        }})
    );

    const Vec4 v{75, 110, 350, 250};
    const Vec2 pad{10, 10};
    m_list = std::make_unique<List>(3, 6, m_pos, v, pad);

    m_page_index = 0;
    m_pages.resize(1);
    PackListDownload();
}

Menu::~Menu() {

}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (m_pages.empty()) {
        return;
    }

    const auto& page = m_pages[m_page_index];
    if (page.m_ready != PageLoadState::Done) {
        return;
    }

    m_list->OnUpdate(controller, touch, page.m_packList.size(), [this](auto i) {
        if (m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect_Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_pages.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty!"_i18n.c_str());
        return;
    }

    auto& page = m_pages[m_page_index];

    switch (page.m_ready) {
        case PageLoadState::None:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Not Ready..."_i18n.c_str());
            return;
        case PageLoadState::Loading:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Loading"_i18n.c_str());
            return;
        case PageLoadState::Done:
            break;
        case PageLoadState::Error:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Error loading page!"_i18n.c_str());
            return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    m_list->Draw(vg, theme, page.m_packList.size(), [this, &page, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        auto& e = page.m_packList[pos];

        auto text_id = ThemeEntryID_TEXT;
        if (pos == m_index) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            DrawElement(x, y, w, h, ThemeEntryID_GRID);
        }

        const float xoff = (350 - 320) / 2;

        // lazy load image
        if (e.themes.size()) {
            auto& theme = e.themes[0];
            auto& image = e.themes[0].preview.lazy_image;

            // try and load cached image.
            if (image_load_count < image_load_max && !image.image && !image.tried_cache) {
                image.tried_cache = true;
                image.cached = loadThemeImage(theme);
                if (image.cached) {
                    image_load_count++;
                }
            }

            if (!image.image || image.cached) {
                switch (image.state) {
                    case ImageDownloadState::None: {
                        const auto path = apiBuildIconCache(theme);
                        log_write("downloading theme!: %s\n", path.s);

                        const auto url = theme.preview.thumb;
                        log_write("downloading url: %s\n", url.c_str());
                        image.state = ImageDownloadState::Progress;
                        curl::Api().ToFileAsync(
                            curl::Url{url},
                            curl::Path{path},
                            curl::Flags{curl::Flag_Cache},
                            curl::StopToken{this->GetToken()},
                            curl::OnComplete{[this, &image](auto& result) {
                                if (result.success) {
                                    image.state = ImageDownloadState::Done;
                                    // data hasn't changed
                                    if (result.code == 304) {
                                        image.cached = false;
                                    }
                                } else {
                                    image.state = ImageDownloadState::Failed;
                                    log_write("failed to download image\n");
                                }
                            }
                        });
                    }   break;
                    case ImageDownloadState::Progress: {

                    }   break;
                    case ImageDownloadState::Done: {
                        image.cached = false;
                        if (!loadThemeImage(theme)) {
                            image.state = ImageDownloadState::Failed;
                        } else {
                            image_load_count++;
                        }
                    }   break;
                    case ImageDownloadState::Failed: {
                    }   break;
                }
            }

            gfx::drawImageRounded(vg, x + xoff, y, 320, 180, image.image ? image.image : App::GetDefaultImage());
        }

        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, w - 30.f, h); // clip
        {
            gfx::drawTextArgs(vg, x + xoff, y + 180 + 20, 18, NVG_ALIGN_LEFT, theme->GetColour(text_id), "%s", e.details.name.c_str());
            gfx::drawTextArgs(vg, x + xoff, y + 180 + 55, 18, NVG_ALIGN_LEFT, theme->GetColour(text_id), "%s", e.creator.display_name.c_str());
        }
        nvgRestore(vg);
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

void Menu::InvalidateAllPages() {
    m_pages.clear();
    m_pages.resize(1);
    m_page_index = 0;
    PackListDownload();
}

void Menu::PackListDownload() {
    const auto page_index = m_page_index + 1;
    char subheading[128];
    std::snprintf(subheading, sizeof(subheading), "Page %zu / %zu"_i18n.c_str(), m_page_index+1, m_page_index_max);
    SetSubHeading(subheading);

    m_index = 0;
    m_list->SetYoff(0);

    // already downloaded
    if (m_pages[m_page_index].m_ready != PageLoadState::None) {
        return;
    }
    m_pages[m_page_index].m_ready = PageLoadState::Loading;

    Config config;
    config.page = page_index;
    config.SetQuery(m_search);
    config.sort_index = m_sort.Get();
    config.order_index = m_order.Get();
    config.nsfw = m_nsfw.Get();
    const auto packList_url = apiBuildUrlListPacks(config);
    const auto packlist_path = apiBuildListPacksCache(config);

    log_write("\npackList_url: %s\n\n", packList_url.c_str());

    curl::Api().ToFileAsync(
        curl::Url{packList_url},
        curl::Path{packlist_path},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this, page_index](auto& result){
            log_write("got themezer data\n");
            if (!result.success) {
                auto& page = m_pages[page_index-1];
                page.m_ready = PageLoadState::Error;
                log_write("failed to get themezer data...\n");
                return;
            }

            PackList a;
            from_json(result.path, a);

            m_pages.resize(a.pagination.page_count);
            auto& page = m_pages[page_index-1];

            page.m_packList = a.packList;
            page.m_pagination = a.pagination;
            page.m_ready = PageLoadState::Done;
            m_page_index_max = a.pagination.page_count;

            char subheading[128];
            std::snprintf(subheading, sizeof(subheading), "Page %zu / %zu"_i18n.c_str(), m_page_index+1, m_page_index_max);
            SetSubHeading(subheading);

            log_write("a.pagination.page: %zu\n", a.pagination.page);
            log_write("a.pagination.page_count: %zu\n", a.pagination.page_count);
        }
    });
}

} // namespace sphaira::ui::menu::themezer
