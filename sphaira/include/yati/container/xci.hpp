#pragma once

#include "base.hpp"
#include <vector>
#include <memory>
#include <switch.h>

namespace sphaira::yati::container {

struct Xci final : Base {
    using Base::Base;
    Result GetCollections(Collections& out) override;
    static Result Validate(source::Base* source);
};

} // namespace sphaira::yati::container
