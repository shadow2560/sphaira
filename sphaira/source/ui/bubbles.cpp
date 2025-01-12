#include "ui/types.hpp"
#include "ui/object.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"

namespace sphaira::ui::bubble {
namespace {

constexpr auto MAX_BUBBLES = 20;

struct Bubble {
    int start_x;
    int texture;
    int x,y,w,h;
    int y_inc;
    float sway_inc;
    float sway;
    bool sway_right_flag;
};

Bubble bubbles[MAX_BUBBLES]{};
int g_textures[3];
bool g_is_init = false;

void setup_bubble(Bubble *bubble) {
    // setup normal vars.
    bubble->texture = (randomGet64() % std::size(g_textures));
    bubble->start_x = randomGet64() % (int)SCREEN_WIDTH;
    bubble->x = bubble->start_x;
    bubble->y = (int)SCREEN_HEIGHT - ( randomGet64() % 60 );
    const int size = (randomGet64() % 50) + 40;
    bubble->w = size;
    bubble->h = size;
    bubble->y_inc = (randomGet64() % 5) + 1;
    bubble->sway_inc = ((randomGet64() % 6) + 3) / 10;
    bubble->sway = 0;
}

void setup_bubbles(void) {
    for (auto& bubble : bubbles) {
        setup_bubble(&bubble);
    }
}

void update_bubbles(void) {
    for (auto& bubble : bubbles) {
        if (bubble.y + bubble.h < 0) {
            setup_bubble(&bubble);
        } else {
            bubble.y -= bubble.y_inc;

            if (bubble.sway_right_flag) {
                bubble.x = bubble.start_x + (bubble.sway -= bubble.sway_inc);
                if (bubble.sway <= 0) {
                    bubble.sway_right_flag = false;
                }
            } else {
                bubble.x = bubble.start_x + (bubble.sway += bubble.sway_inc);
                if (bubble.sway > 30) {
                    bubble.sway_right_flag = true;
                }
            }
        }
    }
}

} // namespace

void Init() {
    if (g_is_init) {
        return;
    }

    if (R_SUCCEEDED(romfsInit())) {
        ON_SCOPE_EXIT(romfsExit());

        auto vg = App::GetVg();
        g_textures[0] = nvgCreateImage(vg, "romfs:/theme/bubble1.png", 0);
        g_textures[1] = nvgCreateImage(vg, "romfs:/theme/bubble2.png", 0);
        g_textures[2] = nvgCreateImage(vg, "romfs:/theme/bubble3.png", 0);

        setup_bubbles();
        g_is_init = true;
    }
}

void Draw(NVGcontext* vg, Theme* theme) {
    if (!g_is_init) {
        return;
    }

    update_bubbles();

    for (auto& bubble : bubbles) {
        gfx::drawImage(vg, bubble.x, bubble.y, bubble.w, bubble.h, g_textures[bubble.texture]);
    }
}

void Exit() {
    if (!g_is_init) {
        return;
    }

    auto vg = App::GetVg();
    for (auto& texture : g_textures) {
        if (texture) {
            nvgDeleteImage(vg, texture);
            texture = 0;
        }
    }

    g_is_init = false;
}

} // namespace sphaira::ui::bubble
