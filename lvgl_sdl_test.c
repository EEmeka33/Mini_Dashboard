#include <unistd.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "lvgl.h"


static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;

#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf)
{
    LV_UNUSED(level);
    LV_UNUSED(buf);
}
#endif

int main()
{
    /* Initialize LVGL */
    lv_init();

    #ifndef WIN32
        setenv("DBUS_FATAL_WARNINGS", "0", 1);
    #endif

    #if LV_USE_LOG != 0
        lv_log_register_print_cb(lv_log_print_g_cb);
    #endif

    /* Create SDL display and input devices */
    lvDisplay = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    lvMouse = lv_sdl_mouse_create();
    lvMouseWheel = lv_sdl_mousewheel_create();
    lvKeyboard = lv_sdl_keyboard_create();

    /* Create a simple widget to test */
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello LVGL!");
    lv_obj_center(label);

    /* Main loop */
    Uint32 lastTick = SDL_GetTicks();
    while(1) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type == SDL_QUIT) return 0;
        }

        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;

        lv_timer_handler();  // Update the UI
        SDL_Delay(5);
    }

    return 0;
}
