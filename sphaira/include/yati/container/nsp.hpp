#pragma once

#include "base.hpp"
#include <switch.h>

namespace sphaira::yati::container {

struct Nsp final : Base {
    using Base::Base;
    Result GetCollections(Collections& out) override;
    static Result Validate(source::Base* source);
};

} // namespace sphaira::yati::container
