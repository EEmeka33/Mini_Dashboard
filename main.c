/* main.c
 * LVGL 9.3 + SDL2 speedometer without canvas.
 * Uses lv_arc for colored ranges and lv_line for the needle.
 *
 * Build example:
 * gcc main.c $(find ./lvgl/src -name '*.c' | grep -v '/test/' | tr '\n' ' ') \
 *   -I. -I./lvgl -I./lvgl/src -I./lvgl/src/drivers/sdl `sdl2-config --cflags --libs` -lm -O2 -o app
 *
 * Make sure LVGL has ARC, LINE and LABEL enabled in lv_conf.h:
 *   #define LV_USE_ARC 1
 *   #define LV_USE_LINE 1
 *   #define LV_USE_LABEL 1
 *
 * Put this file next to your lvgl tree and run the compile line above
 * (or integrate into your existing Makefile).
 */

#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_mousewheel.h" /* optional */
#include "drivers/sdl/lv_sdl_keyboard.h"   /* optional */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- Config ---------- */
#ifndef SDL_HOR_RES
#define SDL_HOR_RES 800
#endif
#ifndef SDL_VER_RES
#define SDL_VER_RES 480
#endif

/* ---------- pointer read (reads SDL_GetMouseState) ---------- */
static void pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    (void)indev;
    int mx = 0, my = 0;
    uint32_t buttons = SDL_GetMouseState(&mx, &my);
    data->point.x = mx;
    data->point.y = my;
    data->state = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

/* ---------- speedometer struct & helpers ---------- */
typedef struct {
    lv_obj_t * container;    /* square parent that holds everything */
    lv_obj_t * base_arc;     /* grey background arc */
    lv_obj_t ** range_arcs;  /* array of arcs for colored ranges */
    int ranges_cnt;
    lv_obj_t * needle_line;  /* lv_line used as needle */
    lv_obj_t * value_label;  /* numeric label */
    int diameter;            /* px (container size) */
    int cx, cy;              /* center coordinates inside container */
    int minv, maxv;
    int start_angle;         /* degrees start */
    int sweep;               /* degrees sweep */
} speedo_t;

typedef struct {
    int start;      /* inclusive */
    int end;        /* inclusive */
    lv_color_t color;
} range_cfg_t;

/* Map value to angle (degrees) */
static float value_to_angle(const speedo_t * s, int v)
{
    if (v < s->minv) v = s->minv;
    if (v > s->maxv) v = s->maxv;
    float frac = (float)(v - s->minv) / (float)(s->maxv - s->minv);
    return (float)s->start_angle + frac * (float)s->sweep;
}

/* Convert degrees to radians */
static inline float deg2rad(float d) { return d * (M_PI / 180.0f); }

/* ---------- create a speedometer widget (no canvas) ---------- */
/* parent: parent object (e.g. lv_scr_act())
 * diameter: px size (square) of the instrument
 * minv,maxv: value range
 * ranges: array of range_cfg_t
 * ranges_cnt: length of ranges
 * start_angle_deg: start angle in degrees (0 = right, 90 = down)
 * sweep_deg: number of degrees the gauge sweeps (e.g. 300)
 */
speedo_t * create_speedometer(lv_obj_t * parent,
                              int diameter,
                              int minv, int maxv,
                              range_cfg_t * ranges, int ranges_cnt,
                              int start_angle_deg, int sweep_deg)
{
    if (!parent || diameter <= 0 || ranges_cnt < 0) return NULL;

    speedo_t * s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->diameter = diameter;
    s->minv = minv;
    s->maxv = maxv;
    s->ranges_cnt = ranges_cnt;
    s->start_angle = start_angle_deg;
    s->sweep = sweep_deg;

    s->container = lv_obj_create(parent);
    lv_obj_set_size(s->container, diameter, diameter);
    lv_obj_center(s->container);

    s->cx = diameter / 2;
    s->cy = diameter / 2;

    /* base arc (grey) spanning the entire sweep */
    s->base_arc = lv_arc_create(s->container);
    lv_obj_set_size(s->base_arc, diameter, diameter);
    lv_obj_align(s->base_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(s->base_arc, minv, maxv);
    /* set the background arc (the visible sweep) */
    lv_arc_set_bg_angles(s->base_arc, start_angle_deg, start_angle_deg + sweep_deg);
    /* style: thin grey arc as background */
    lv_obj_set_style_arc_width(s->base_arc, diameter / 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->base_arc, lv_color_hex(0x444444), LV_PART_MAIN);
    /* make the indicator invisible (we only use background arcs here) */
    lv_obj_set_style_arc_color(s->base_arc, lv_color_hex(0x222222), LV_PART_INDICATOR);

    /* create per-range arcs (drawn on top of base) */
    s->range_arcs = calloc(ranges_cnt, sizeof(lv_obj_t *));
    for (int i = 0; i < ranges_cnt; ++i) {
        range_cfg_t * rc = &ranges[i];
        lv_obj_t * a = lv_arc_create(s->container);
        lv_obj_set_size(a, diameter, diameter);
        lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_range(a, minv, maxv);
        /* set the background angles to match range start..end (in degrees) */
        /* compute angles from values */
        /* angle = start_angle + frac * sweep */
        float a0f = value_to_angle(s, rc->start);
        float a1f = value_to_angle(s, rc->end);
        lv_arc_set_bg_angles(a, (int)a0f, (int)a1f);
        lv_obj_set_style_arc_width(a, diameter / 12, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, rc->color, LV_PART_MAIN);
        /* hide indicator */
        lv_obj_set_style_arc_color(a, lv_color_make(0,0,0), LV_PART_INDICATOR);
        s->range_arcs[i] = a;
    }

    /* needle: use lv_line and update its end point on set_speed.
       We'll create a line that we'll redraw by setting new points. */
    s->needle_line = lv_line_create(s->container);
    /* initially draw as a zero-length line at center; we'll update points in set_speed */
    lv_point_t pts[2] = { {s->cx, s->cy}, {s->cx, s->cy} };
    lv_line_set_points(s->needle_line, pts, 2);
    /* style the needle */
    lv_obj_set_style_line_width(s->needle_line, diameter / 80, LV_PART_MAIN);
    lv_obj_set_style_line_color(s->needle_line, lv_color_hex(0xDD3333), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s->needle_line, true, LV_PART_MAIN);
    /* line object default pos is 0,0 inside container which is fine because points are container-local */

    /* center pivot: small circle */
    lv_obj_t * pivot = lv_obj_create(s->container);
    int pivot_sz = diameter / 16;
    lv_obj_set_size(pivot, pivot_sz, pivot_sz);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_center(pivot);
    lv_obj_set_style_bg_color(pivot, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_set_style_border_width(pivot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(pivot, lv_color_hex(0x333333), LV_PART_MAIN);

    /* numeric label below center */
    s->value_label = lv_label_create(s->container);
    lv_label_set_text_fmt(s->value_label, "%d", minv);
    lv_obj_align(s->value_label, LV_ALIGN_BOTTOM_MID, 0, - (diameter / 12));
    lv_obj_set_style_text_font(s->value_label, &lv_font_montserrat_22, LV_PART_MAIN);

    /* initial needle at min value */
    /* we'll call set_speed externally to set initial position if desired */

    return s;
}

/* Update the speed (needle + numeric label). No animation in this simple version.
 * For smooth motion add an lv_anim on a variable and call this from the anim callback.
 */
void set_speed(speedo_t * s, int value)
{
    if (!s) return;
    if (value < s->minv) value = s->minv;
    if (value > s->maxv) value = s->maxv;

    float ang = value_to_angle(s, value); /* degrees */
    float r = (s->diameter / 2) - (s->diameter / 6); /* needle length */
    float rad = deg2rad(ang);
    int nx = s->cx + (int)roundf(r * cosf(rad));
    int ny = s->cy + (int)roundf(r * sinf(rad));

    /* line points are in container-local coords; set new points */
    lv_point_t pts[2] = { {s->cx, s->cy}, {nx, ny} };
    lv_line_set_points(s->needle_line, pts, 2);

    /* update numeric label */
    lv_label_set_text_fmt(s->value_label, "%d", value);

    /* ask LVGL to refresh container */
    lv_obj_invalidate(s->container);
}

/* destroy helper */
void destroy_speedo(speedo_t * s)
{
    if (!s) return;
    if (s->range_arcs) free(s->range_arcs);
    if (s->container) lv_obj_del(s->container);
    free(s);
}

/* ---------- example main ---------- */
int main(void)
{
    /* 1) Init SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* 2) Init LVGL */
    lv_init();

#ifndef WIN32
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
#endif

    /* Create display */
    lv_display_t * disp = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        SDL_Quit();
        return 1;
    }

    (void)lv_sdl_mousewheel_create();
    (void)lv_sdl_keyboard_create();

    /* pointer indev */
    lv_indev_t * my_mouse = lv_indev_create();
    if (my_mouse) {
        lv_indev_set_type(my_mouse, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(my_mouse, pointer_read_cb);
    }
    SDL_ShowCursor(SDL_ENABLE);

    /* Build UI */
    lv_obj_t * scr = lv_scr_act();

    /* Example ranges: green 0..60, yellow 61..140, red 141..240 */
    range_cfg_t ranges[3];
    ranges[0].start = 0;   ranges[0].end = 60;  ranges[0].color = lv_color_hex(0x24A148);
    ranges[1].start = 61;  ranges[1].end = 140; ranges[1].color = lv_color_hex(0xF6C000);
    ranges[2].start = 141; ranges[2].end = 240; ranges[2].color = lv_color_hex(0xD72638);

    speedo_t * speedo = create_speedometer(scr, 380, 0, 240, ranges, 3, 120, 300);
    if (!speedo) {
        fprintf(stderr, "create_speedometer failed\n");
        SDL_Quit();
        return 1;
    }

    /* demo sweep */
    int v = 0, dir = 1;
    Uint32 lastTick = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
        }

        Uint32 now = SDL_GetTicks();
        uint32_t diff = now - lastTick;
        if (diff) {
            lv_tick_inc(diff);
            lastTick = now;
        }

        /* simple demo increment */
        v += dir;
        if (v >= 240) { v = 240; dir = -1; }
        if (v <= 0)   { v = 0;   dir = 1; }

        set_speed(speedo, v);

        lv_timer_handler();
        SDL_Delay(16);
    }

    destroy_speedo(speedo);
    SDL_Quit();
    return 0;
}
