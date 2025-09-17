/* main.c
   LVGL 9.x + SDL example: interactive slider using a custom LVGL pointer indev
   that reads SDL_GetMouseState() directly.
*/

#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"  /* optional */
#include "drivers/sdl/lv_sdl_keyboard.h"    /* optional */

#ifndef SDL_HOR_RES
#define SDL_HOR_RES 800
#endif
#ifndef SDL_VER_RES
#define SDL_VER_RES 480
#endif

/* ---------- Custom pointer read callback (reads SDL mouse state) ---------- */
static void pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    (void)indev;
    int mx = 0, my = 0;
    uint32_t buttons = SDL_GetMouseState(&mx, &my); /* returns x,y in window coords */

    /* If you use scaling or a different coord system, transform (mx,my) here to LVGL coords */
    data->point.x = mx;
    data->point.y = my;

    /* Map left button to LV pressed state */
    data->state = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    data->continue_reading = false;
}

/* ---------- Slider event callback ---------- */
static void slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    lv_obj_t * val_label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t * bar = (lv_obj_t *)lv_obj_get_user_data(slider);

    int32_t v = lv_slider_get_value(slider);
    char buf[32];
    snprintf(buf, sizeof(buf), "Value: %d", (int)v);
    lv_label_set_text(val_label, buf);

    if (bar) lv_bar_set_value(bar, v, LV_ANIM_ON);
}

int main(void)
{
    /* Initialize SDL (needed by lv_sdl driver and SDL_GetMouseState) */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Initialize LVGL */
    lv_init();

    #ifndef WIN32
        setenv("DBUS_FATAL_WARNINGS", "0", 1);
    #endif

    /* Create SDL window + LVGL display using existing driver */
    lv_display_t * disp = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        SDL_Quit();
        return 1;
    }

    /* Create other input devices if you want (wheel/keyboard) */
    (void)lv_sdl_mousewheel_create(); /* optional: not necessary for pointer */
    (void)lv_sdl_keyboard_create();   /* optional */

    /* Create a custom LVGL pointer input device that reads SDL_GetMouseState directly */
    lv_indev_t * my_mouse = lv_indev_create();
    if (!my_mouse) {
        fprintf(stderr, "lv_indev_create failed\n");
        /* still continue, but no pointer will work */
    } else {
        lv_indev_set_type(my_mouse, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(my_mouse, pointer_read_cb);
    }

    /* Optionally show the OS cursor so you can see where the pointer is */
    SDL_ShowCursor(SDL_ENABLE);

    /* --- Build UI: slider + progress bar + label --- */
    lv_obj_t * scr = lv_scr_act();

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Interactive Slider (custom pointer)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t * slider = lv_slider_create(scr);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_set_width(slider, LV_PCT(70));
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t * bar = lv_bar_create(scr);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, lv_slider_get_value(slider), LV_ANIM_OFF);
    lv_obj_set_width(bar, LV_PCT(70));
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t * value_label = lv_label_create(scr);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "Value: %d", (int)lv_slider_get_value(slider));
    lv_label_set_text(value_label, tmp);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, -60);

    /* Link slider <-> bar and register event */
    lv_obj_set_user_data(slider, bar); /* store pointer to bar inside slider */
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, value_label);

    /* Main loop */
    Uint32 lastTick = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
            /* Do not consume mouse events here — polling is enough for SDL_GetMouseState() */
        }

        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;

        lv_timer_handler();

        SDL_Delay(1); /* keep loop responsive */
    }

    /* Cleanup */
    /* NOTE: lv_display_delete()/lv_indev_delete() may or may not exist depending on LVGL build.
       If your build provides them, call them. Otherwise freeing SDL and exiting is fine. */
    SDL_Quit();
    return 0;
}
