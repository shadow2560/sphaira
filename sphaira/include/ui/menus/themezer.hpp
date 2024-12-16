#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/scrollable_text.hpp"
#include "option.hpp"
#include <span>

namespace sphaira::ui::menu::themezer {

enum class ImageDownloadState {
    None, // not started
    Progress, // Download started
    Done, // finished downloading
    Failed, // attempted to download but failed
};

struct LazyImage {
    LazyImage() = default;
    ~LazyImage();
    int image{};
    int w{}, h{};
    ImageDownloadState state{ImageDownloadState::None};
    u8 first_pixel[4]{};
};

// "mutation setLike($type: String!, $id: String!, $value: Boolean!) {\n setLike(type: $type, id: $id, value: $value)\n}\n"

// https://api.themezer.net/?query=query($nsfw:Boolean,$target:String,$page:Int,$limit:Int,$sort:String,$order:String,$query:String){themeList(nsfw:$nsfw,target:$target,page:$page,limit:$limit,sort:$sort,order:$order,query:$query){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}&variables={"nsfw":false,"target":null,"page":1,"limit":10,"sort":"updated","order":"desc","query":null}
// https://api.themezer.net/?query=query($nsfw:Boolean,$page:Int,$limit:Int,$sort:String,$order:String,$query:String){packList(nsfw:$nsfw,page:$page,limit:$limit,sort:$sort,order:$order,query:$query){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,themes{id,creator{display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}}&variables={"nsfw":false,"page":1,"limit":10,"sort":"updated","order":"desc","query":null}
// https://api.themezer.net/?query=query($id:String!){pack(id:$id){id,creator{display_name},details{name,description},last_updated,categories,dl_count,like_count,themes{id,details{name},layout{id,details{name}},categories,target,preview{original,thumb},last_updated,dl_count,like_count}}}&variables={"id":"16d"}

// https://api.themezer.net/?query=query{nxinstaller(id:"t9a6"){themes{filename,url,mimetype}}}
// https://api.themezer.net/?query=query{downloadTheme(id:"t9a6"){filename,url,mimetype}}
// https://api.themezer.net/?query=query{downloadPack(id:"t9a6"){filename,url,mimetype}}

// {"data":{"setLike":true}}
// https://api.themezer.net/?query=mutation{setLike(type:"packs",id:"5",value:true){data{setLike}}}
// https://api.themezer.net/?query=mutation($type:String!,$id:String!,$value:Boolean!){setLike(type:$type,id:$id,value:$value){data{setLike}}}&variables={"type":"packs","id":"5","value":true}

enum MenuState {
    MenuState_Normal,
    MenuState_Search,
    MenuState_Creator,
};

enum ListType {
    ListType_Pack, // list complete packs
    ListType_Target, // list types
};

enum class PageLoadState {
    None,
    Loading,
    Done,
    Error,
};

struct Creator {
    std::string id;
    std::string display_name;
};

struct Details {
    std::string name;
    std::string description;
};

struct Preview {
    std::string original;
    std::string thumb;
    LazyImage lazy_image;
};

struct DownloadPack {
    std::string filename;
    std::string url;
    std::string mimetype;
};

using DownloadTheme = DownloadPack;

struct ThemeEntry {
    std::string id;
    Creator creator;
    Details details;
    std::string last_updated;
    u64 dl_count;
    u64 like_count;
    std::vector<std::string> categories;
    std::string target;
    Preview preview;
};

// struct Pack {
//     std::string id;
//     Creator creator;
//     Details details;
//     std::string last_updated;
//     std::vector<std::string> categories;
//     u64 dl_count;
//     u64 like_count;
//     std::vector<ThemeEntry> themes;
// };

struct PackListEntry {
    std::string id;
    Creator creator;
    Details details;
    std::string last_updated;
    std::vector<std::string> categories;
    u64 dl_count;
    u64 like_count;
    std::vector<ThemeEntry> themes;
};

struct Pagination {
    u64 page;
    u64 limit;
    u64 page_count;
    u64 item_count;
};

struct PackList {
    std::vector<PackListEntry> packList;
    Pagination pagination;
};

struct Config {
    // these index into a string array
    u32 target_index{};
    u32 sort_index{};
    u32 order_index{};
    // search query, if empty, its not used
    std::string query;
    // this is actually an array of creator ids, but we don't support that feature
    // if empty, its not used
    std::string creator;
    // defaults
    u32 page{1};
    u32 limit{18};
    bool nsfw{false};

    void SetQuery(std::string new_query) {
        query = new_query;
    }

    void RemoveQuery() {
        query.clear();
    }

    void SetCreator(Creator new_creator) {
        creator = new_creator.id;
    }

    void RemoveCreator() {
        creator.clear();
    }
};

struct Menu; // fwd

struct PageEntry {
    std::vector<PackListEntry> m_packList;
    Pagination m_pagination{};
    PageLoadState m_ready{PageLoadState::None};
};

struct Menu final : MenuBase {
    Menu();
    ~Menu();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

    void SetIndex(std::size_t index) {
        m_index = index;
    }

    // void SetSearch(const std::string& term);
    // void SetAuthor();

    void InvalidateAllPages();
    void PackListDownload();
    void OnPackListDownload();

private:
    static constexpr inline const char* INI_SECTION = "themezer";
    static constexpr inline u32 MAX_ON_PAGE = 16; // same as website

    std::vector<PageEntry> m_pages;
    std::size_t m_page_index{};
    std::size_t m_page_index_max{1};

    std::string m_search{};

    std::size_t m_start{};
    std::size_t m_index{}; // where i am in the array

    // options
    option::OptionLong m_sort{INI_SECTION, "sort", 0};
    option::OptionLong m_order{INI_SECTION, "order", 0};
    option::OptionBool m_nsfw{INI_SECTION, "nsfw", false};
};

} // namespace sphaira::ui::menu::themezer
