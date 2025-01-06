#pragma once

#include "nanovg.h"
#include "ui/types.hpp"

namespace sphaira::ui::gfx {

enum class Colour {
    BLACK,
    LIGHT_BLACK,
    SILVER,
    DARK_GREY,
    GREY,
    WHITE,
    CYAN,
    TEAL,
    BLUE,
    LIGHT_BLUE,
    YELLOW,
    RED,
};

void drawImage(NVGcontext*, float x, float y, float w, float h, int texture);
void drawImage(NVGcontext*, Vec4 v, int texture);
void drawImageRounded(NVGcontext*, float x, float y, float w, float h, int texture);
void drawImageRounded(NVGcontext*, Vec4 v, int texture);

auto getColour(Colour c) -> NVGcolor;

void dimBackground(NVGcontext*);

void drawRect(NVGcontext*, float x, float y, float w, float h, Colour c, bool rounded = false);
void drawRect(NVGcontext*, Vec4 vec, Colour c, bool rounded = false);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGcolor& c, bool rounded = false);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGcolor&& c, bool rounded = false);
void drawRect(NVGcontext*, Vec4 vec, const NVGcolor& c, bool rounded = false);
void drawRect(NVGcontext*, Vec4 vec, const NVGcolor&& c, bool rounded = false);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGpaint& p, bool rounded = false);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGpaint&& p, bool rounded = false);
void drawRect(NVGcontext*, Vec4 vec, const NVGpaint& p, bool rounded = false);
void drawRect(NVGcontext*, Vec4 vec, const NVGpaint&& p, bool rounded = false);

void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, float x, float y, float w, float h, Colour c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, Vec4 vec, Colour c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, float x, float y, float w, float h, const NVGcolor& c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, float x, float y, float w, float h, const NVGcolor&& c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, Vec4 vec, const NVGcolor& c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, Vec4 vec, const NVGcolor&& c);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, float x, float y, float w, float h, const NVGpaint& p);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, float x, float y, float w, float h, const NVGpaint&& p);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, Vec4 vec, const NVGpaint& p);
void drawRectOutline(NVGcontext*, float size, const NVGcolor& out_col, Vec4 vec, const NVGpaint&& p);

void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, Colour c);
void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGcolor& c);
void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGcolor&& c);
void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGpaint& p);
void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGpaint&& p);

void drawText(NVGcontext*, float x, float y, float size, const char* str, const char* end, int align, Colour c);
void drawText(NVGcontext*, float x, float y, float size, Colour c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, Vec2 vec, float size, const char* str, const char* end, int align, Colour c);
void drawText(NVGcontext*, Vec2 vec, float size, Colour c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, float x, float y, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, float x, float y, float size, const char* str, const char* end, int align, const NVGcolor&& c);
void drawText(NVGcontext*, float x, float y, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, float x, float y, float size, const NVGcolor&& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, Vec2 vec, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, Vec2 vec, float size, const char* str, const char* end, int align, const NVGcolor&& c);
void drawText(NVGcontext*, Vec2 vec, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, Vec2 vec, float size, const NVGcolor&& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawTextArgs(NVGcontext*, float x, float y, float size, int align, Colour c, const char* str, ...) __attribute__ ((format (printf, 7, 8)));
void drawTextArgs(NVGcontext*, float x, float y, float size, int align, const NVGcolor& c, const char* str, ...) __attribute__ ((format (printf, 7, 8)));

void drawTextBox(NVGcontext*, float x, float y, float size, float bound, NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawTextBox(NVGcontext*, float x, float y, float size, float bound, NVGcolor&& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawTextBox(NVGcontext*, float x, float y, float size, float bound, Colour c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);

void textBounds(NVGcontext*, float x, float y, float *bounds, const char* str, ...) __attribute__ ((format (printf, 5, 6)));
// void textBounds(NVGcontext*, float *bounds, const char* str, ...) __attribute__ ((format (printf, 5, 6)));
// void textBounds(NVGcontext*, float *bounds, const char* str);

auto getButton(Button button) -> const char*;
void drawScrollbar(NVGcontext* vg, Theme* theme, u32 index_off, u32 count, u32 max_per_page);
void drawScrollbar(NVGcontext* vg, Theme* theme, float x, float y, float h, u32 index_off, u32 count, u32 max_per_page);

void drawScrollbar2(NVGcontext* vg, Theme* theme, float x, float y, float h, s64 index_off, s64 count, s64 row, s64 page);
void drawScrollbar2(NVGcontext* vg, Theme* theme, s64 index_off, s64 count, s64 row, s64 page);

void updateHighlightAnimation();
void getHighlightAnimation(float* gradientX, float* gradientY, float* color);

} // namespace sphaira::ui::gfx
