#pragma once

#include <switch.h>
#include <string>

namespace sphaira {

// if show_error = true, it will display popup error box on
// faliure. set this to false if you want to handle errors
// from the caller.
auto WebShow(const std::string& url, bool show_error = true) -> Result;

} // namespace sphaira
