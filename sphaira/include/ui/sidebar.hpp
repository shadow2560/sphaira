#pragma once

#include "ui/widget.hpp"
#include "ui/list.hpp"
#include <memory>
#include <concepts>
#include <utility>

namespace sphaira::ui {

class SidebarEntryBase : public Widget {
public:
    SidebarEntryBase(std::string&& title);
    virtual auto Draw(NVGcontext* vg, Theme* theme) -> void override;

protected:
    std::string m_title;
};

template<typename T>
concept DerivedFromSidebarBase = std::is_base_of_v<SidebarEntryBase, T>;

class SidebarEntryBool final : public SidebarEntryBase {
public:
    using Callback = std::function<void(bool&)>;

public:
    SidebarEntryBool(std::string title, bool option, Callback cb, std::string true_str = "On", std::string false_str = "Off");
    SidebarEntryBool(std::string title, bool& option, std::string true_str = "On", std::string false_str = "Off");

private:
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    bool m_option;
    Callback m_callback;
    std::string m_true_str;
    std::string m_false_str;
};

class SidebarEntryCallback final : public SidebarEntryBase {
public:
    using Callback = std::function<void()>;

public:
    SidebarEntryCallback(std::string title, Callback cb, bool pop_on_click = false);
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

private:
    Callback m_callback;
    bool m_pop_on_click;
};

class SidebarEntryArray final : public SidebarEntryBase {
public:
    using Items = std::vector<std::string>;
    using ListCallback = std::function<void()>;
    using Callback = std::function<void(s64& index)>;

public:
    explicit SidebarEntryArray(std::string title, Items items, Callback cb, s64 index = 0);
    SidebarEntryArray(std::string title, Items items, Callback cb, std::string index);
    SidebarEntryArray(std::string title, Items items, std::string& index);

    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

private:
    Items m_items;
    ListCallback m_list_callback;
    Callback m_callback;
    s64 m_index;
    s64 m_tick{};
    float m_text_yoff{};
};

template <typename T>
class SidebarEntrySlider final : public SidebarEntryBase {
public:
    SidebarEntrySlider(std::string title, T& value, T min, T max)
    : SidebarEntryBase{title}
    , m_value{value}
    , m_min{min}
    , m_max{max} {

    }

    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto Update(Controller* controller, TouchInfo* touch) -> void override;

private:
    T& m_value;
    T m_min;
    T m_max;
    T m_step{};
    Vec4 m_bar{};
    Vec4 m_bar_fill{};
};

class Sidebar final : public Widget {
public:
    enum class Side { LEFT, RIGHT };
    using Items = std::vector<std::unique_ptr<SidebarEntryBase>>;

public:
    Sidebar(std::string title, Side side, Items&& items);
    Sidebar(std::string title, Side side);
    Sidebar(std::string title, std::string sub, Side side, Items&& items);
    Sidebar(std::string title, std::string sub, Side side);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

    void Add(std::unique_ptr<SidebarEntryBase>&& entry);

    template<DerivedFromSidebarBase T, typename... Args>
    void Add(Args&&... args) {
        Add(std::make_unique<T>(std::forward<Args>(args)...));
    }

private:
    void SetIndex(s64 index);
    void SetupButtons();

private:
    std::string m_title;
    std::string m_sub;
    Side m_side;
    Items m_items;
    s64 m_index{};

    std::unique_ptr<List> m_list;

    Vec4 m_top_bar{};
    Vec4 m_bottom_bar{};
    Vec2 m_title_pos{};
    Vec4 m_base_pos{};

    static constexpr float m_title_size{28.f};
    // static constexpr Vec2 box_size{380.f, 70.f};
    static constexpr Vec2 m_box_size{400.f, 70.f};
};

} // namespace sphaira::ui
