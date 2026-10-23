#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    _SCREEN_ID_LAST = 1
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *main_panel;
    lv_obj_t *stop_button;
    lv_obj_t *start_button;
    lv_obj_t *coating_time_label;
    lv_obj_t *ramp_label;
    lv_obj_t *target_speed_label;
    lv_obj_t *target_speed_value_enter_button;
    lv_obj_t *target_speed_value_lable;
    lv_obj_t *ramp_speed_value_enter_button;
    lv_obj_t *ramp_speed_value_lable;
    lv_obj_t *coating_time_value_enter_button;
    lv_obj_t *coating_time_value_lable;
    lv_obj_t *main_panel_select_option_save_dropdown;
    lv_obj_t *settings_button;
    lv_obj_t *headding;
    lv_obj_t *target_speed_panel;
    lv_obj_t *target_speed_value_text_area;
    lv_obj_t *target_speed_keypad;
    lv_obj_t *ramp_speed_panel;
    lv_obj_t *ramp_speed_value_text_area;
    lv_obj_t *ramp_speed_keypad;
    lv_obj_t *coating_time_panel;
    lv_obj_t *counting_time_value_text_area;
    lv_obj_t *coating_time_keypad;
    lv_obj_t *parameter_save_setting_panel;
    lv_obj_t *save_coating_time_label;
    lv_obj_t *save_ramp_speed_label;
    lv_obj_t *save_target_speed_label;
    lv_obj_t *save_target_speed_value_enter_button;
    lv_obj_t *save_target_speed_value_lable;
    lv_obj_t *save_ramp_speed_value_enter_button;
    lv_obj_t *save_ramp_speed_value_lable;
    lv_obj_t *save_coating_time_value_enter_button;
    lv_obj_t *save_coating_time_value_lable;
    lv_obj_t *save_button;
    lv_obj_t *save_button_lable;
    lv_obj_t *headding_1;
    lv_obj_t *save_option_dropdown;
    lv_obj_t *about_button;
    lv_obj_t *splash_screen;
    lv_obj_t *about_panel;
    lv_obj_t *obj0;
    lv_obj_t *back_button;
    lv_obj_t *obj1;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/