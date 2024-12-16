#pragma once

#include "ui/widget.hpp"
#include <optional>

namespace sphaira::ui {

class OptionList final : public Widget {
public:
    using Options = std::vector<std::pair<std::string, std::function<void()>>>;

public:
    OptionList(Options _options);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto OnLayoutChange() -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

protected:
    Options m_options;
    std::size_t m_index{};

private:

};

} // namespace sphaira::ui
