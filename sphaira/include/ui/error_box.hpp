#pragma once

#include "ui/widget.hpp"
#include <optional>

namespace sphaira::ui {

class ErrorBox final : public Widget {
public:
    ErrorBox(Result code, const std::string& message);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

private:
    Result m_code;
    std::string m_message;
    std::string m_module_str;
    std::string m_description_str;
};

} // namespace sphaira::ui
