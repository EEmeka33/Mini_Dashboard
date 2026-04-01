#include "speedo_interactive.h"
#include <math.h>

#define WIN_SIZE          1080
#define CENTER_X          (WIN_SIZE / 2)
#define CENTER_Y          (WIN_SIZE / 2)
#define SPEED_MIN         0
#define SPEED_MAX         260
#define SPEED_SWEEP_DEG   180.0f
#define SPEED_START_DEG   270.0f

static lv_obj_t * speed_label;
static lv_obj_t * speed_unit_label;
static lv_obj_t * needle_line;
static lv_point_precise_t needle_points[2];

static float speed_to_angle(int32_t speed)
{
    float ratio = (float)(speed - SPEED_MIN) / (float)(SPEED_MAX - SPEED_MIN);
    return SPEED_START_DEG + (ratio * SPEED_SWEEP_DEG);
}

static void update_needle(int32_t speed)
{
    float angle_deg = speed_to_angle(speed);
    float rad = (angle_deg - 90.0f) * (float)M_PI / 180.0f;

    /* Keep the needle only on the border ring */
    int32_t tip_r = 492;
    int32_t base_r = 448;

    needle_points[0].x = CENTER_X + (int32_t)(base_r * cosf(rad));
    needle_points[0].y = CENTER_Y + (int32_t)(base_r * sinf(rad));
    needle_points[1].x = CENTER_X + (int32_t)(tip_r * cosf(rad));
    needle_points[1].y = CENTER_Y + (int32_t)(tip_r * sinf(rad));
    lv_line_set_points(needle_line, needle_points, 2);
}

static void create_ticks_and_labels(lv_obj_t * parent)
{
    static lv_point_precise_t major_pts[27][2];
    static lv_point_precise_t minor_pts[26][2];
    int major_i = 0;
    int minor_i = 0;

    for(int32_t v = SPEED_MIN; v <= SPEED_MAX; v += 5) {
        float angle_deg = speed_to_angle(v);
        float rad = (angle_deg - 90.0f) * (float)M_PI / 180.0f;
        bool major = (v % 10 == 0);

        int32_t outer_r = 499;
        int32_t inner_r = major ? 458 : 472;

        lv_obj_t * tick = lv_line_create(parent);
        lv_point_precise_t * pts;

        if(major) {
            pts = major_pts[major_i++];
        }
        else {
            pts = minor_pts[minor_i++];
        }

        pts[0].x = CENTER_X + (int32_t)(inner_r * cosf(rad));
        pts[0].y = CENTER_Y + (int32_t)(inner_r * sinf(rad));
        pts[1].x = CENTER_X + (int32_t)(outer_r * cosf(rad));
        pts[1].y = CENTER_Y + (int32_t)(outer_r * sinf(rad));
        lv_line_set_points(tick, pts, 2);

        lv_obj_set_style_line_width(tick, major ? 4 : 2, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(major ? 0xE8D7B8 : 0x9A8463), 0);
        lv_obj_set_style_line_rounded(tick, true, 0);

        if(v % 25 == 0) {
            lv_obj_t * num = lv_label_create(parent);
            lv_label_set_text_fmt(num, "%" LV_PRId32, v);
            lv_obj_set_style_text_font(num, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(num, lv_color_hex(0xF2E7D2), 0);
            int32_t text_r = 404;
            lv_obj_set_pos(num,
                           CENTER_X + (int32_t)(text_r * cosf(rad)) - 14,
                           CENTER_Y + (int32_t)(text_r * sinf(rad)) - 12);
        }
    }
}

void speedo_interactive_create(void)
{
    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D0D0D), 0);

    create_ticks_and_labels(scr);

    /* Needle */
    needle_line = lv_line_create(scr);
    lv_obj_set_style_line_width(needle_line, 3, 0);
    lv_obj_set_style_line_color(needle_line, lv_color_hex(0xD94343), 0);
    lv_obj_set_style_line_rounded(needle_line, true, 0);
    update_needle(0);

    speed_label = lv_label_create(scr);
    lv_label_set_text(speed_label, "0");
    lv_obj_set_style_text_font(speed_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(speed_label, lv_color_hex(0xF4E7CF), 0);
    lv_obj_align(speed_label, LV_ALIGN_BOTTOM_MID, 0, -810);

    speed_unit_label = lv_label_create(scr);
    lv_label_set_text(speed_unit_label, "km/h");
    lv_obj_set_style_text_font(speed_unit_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(speed_unit_label, lv_color_hex(0xB89C76), 0);
    lv_obj_align_to(speed_unit_label, speed_label, LV_ALIGN_OUT_BOTTOM_MID, 0, -810);

}

void speedo_set_speed(int32_t speed)
{
    lv_label_set_text_fmt(speed_label, "%" LV_PRId32, speed);
    update_needle(speed);
}
