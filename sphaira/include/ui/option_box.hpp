#pragma once

#include "ui/widget.hpp"
#include <optional>

namespace sphaira::ui {

class OptionBoxEntry final : public Widget {
public:

public:
    OptionBoxEntry(const std::string& text, Vec4 pos);

    auto Update(Controller* controller, TouchInfo* touch) -> void override {}
    auto OnLayoutChange() -> void override {}
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    auto Selected(bool enable) -> void;
private:

private:
    std::string m_text;
    Vec2 m_text_pos{};
    bool m_selected{false};
};

// todo: support multiline messages
// todo: support upto 4 options.
class OptionBox final : public Widget {
public:
    using Callback = std::function<void(std::optional<std::size_t> index)>;
    using Option = std::string;
    using Options = std::vector<Option>;

public:
    OptionBox(const std::string& message, const Option& a, Callback cb); // confirm
    OptionBox(const std::string& message, const Option& a, const Option& b, Callback cb); // yesno
    OptionBox(const std::string& message, const Option& a, const Option& b, std::size_t index, Callback cb); // yesno
    OptionBox(const std::string& message, const Option& a, const Option& b, const Option& c, Callback cb); // tri
    OptionBox(const std::string& message, const Option& a, const Option& b, const Option& c, std::size_t index, Callback cb); // tri

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto OnLayoutChange() -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

private:
    auto Setup(std::size_t index) -> void; // common setup values

private:
    std::string m_message;
    Callback m_callback;

    Vec4 m_spacer_line{};

    std::size_t m_index{};
    std::vector<OptionBoxEntry> m_entries;
};

} // namespace sphaira::ui
