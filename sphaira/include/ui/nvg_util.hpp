#pragma once

#include "nanovg.h"
#include "ui/types.hpp"

namespace sphaira::ui::gfx {

void drawImage(NVGcontext*, float x, float y, float w, float h, int texture);
void drawImage(NVGcontext*, const Vec4& v, int texture);
void drawImageRounded(NVGcontext*, float x, float y, float w, float h, int texture);
void drawImageRounded(NVGcontext*, const Vec4& v, int texture);

void dimBackground(NVGcontext*);

void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGcolor& c, bool rounded = false);
void drawRect(NVGcontext*, const Vec4& v, const NVGcolor& c, bool rounded = false);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGpaint& p, bool rounded = false);
void drawRect(NVGcontext*, const Vec4& v, const NVGpaint& p, bool rounded = false);

void drawRectOutline(NVGcontext*, const Theme*, float size, float x, float y, float w, float h);
void drawRectOutline(NVGcontext*, const Theme*, float size, const Vec4& v);

void drawText(NVGcontext*, float x, float y, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, float x, float y, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, const Vec2& v, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, const Vec2& v, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawTextArgs(NVGcontext*, float x, float y, float size, int align, const NVGcolor& c, const char* str, ...) __attribute__ ((format (printf, 7, 8)));

void drawTextBox(NVGcontext*, float x, float y, float size, float bound, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);

void textBounds(NVGcontext*, float x, float y, float *bounds, const char* str, ...) __attribute__ ((format (printf, 5, 6)));

auto getButton(Button button) -> const char*;
void drawScrollbar(NVGcontext*, const Theme*, u32 index_off, u32 count, u32 max_per_page);
void drawScrollbar(NVGcontext*, const Theme*, float x, float y, float h, u32 index_off, u32 count, u32 max_per_page);

void drawScrollbar2(NVGcontext*, const Theme*, float x, float y, float h, s64 index_off, s64 count, s64 row, s64 page);
void drawScrollbar2(NVGcontext*, const Theme*, s64 index_off, s64 count, s64 row, s64 page);

void updateHighlightAnimation();
void getHighlightAnimation(float* gradientX, float* gradientY, float* color);

} // namespace sphaira::ui::gfx
