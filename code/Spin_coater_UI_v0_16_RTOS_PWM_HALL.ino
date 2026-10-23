#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <lvgl.h>
#include <Preferences.h>
#include <math.h>
  
#include "ui.h"

MCUFRIEND_kbv tft;

#define TOUCH_Y_OFFSET -20


// =====================================================
// SPLASH SCREEN
// =====================================================

#define SPLASH_TIME 5000UL

unsigned long splashStartTime = 0;
bool splashActive = false;





// =====================================================
// LOCAL MOTOR CONTROL
// =====================================================
//
// Motor control runs on this ESP32.
// No motor MCU UART is required.
//
// Hall sensor : GPIO 21
// ESC signal  : GPIO 19
// =====================================================

#define HALL_PIN 21
#define ESC_PIN 19

#define PULSES_PER_REV 2

#define ESC_FREQ 50
#define ESC_RESOLUTION 16
#define ESC_MIN_US 1200
#define ESC_MAX_US 1600

#define RPM_INTERVAL_MS 100
#define CONTROL_INTERVAL_MS 100
#define RPM_FILTER 0.25f

#define FIXED_PWM_STEP 0.5f

float Kp = 0.010f;
float Ki = 0.04f;

volatile unsigned long pulseCount = 0;

float rpm = 0.0f;
float filteredRPM = 0.0f;
float error = 0.0f;
float integral = 0.0f;
float pwmUs = ESC_MIN_US;

unsigned long lastRPMTime = 0;
unsigned long lastControlTime = 0;

TaskHandle_t uiTaskHandle = NULL;
TaskHandle_t motorTaskHandle = NULL;

static void uiTask(void *parameter);
static void motorTask(void *parameter);
static void updateMotorUI();

// =====================================================
// MOTOR RUNNING UI STATE
// =====================================================

static lv_color_t normalTargetButtonColor, normalRampButtonColor, normalCoatingButtonColor;
static lv_color_t normalStartButtonColor, normalStopButtonColor;
static bool normalButtonColorsSaved = false;

static const lv_color_t MOTOR_RUNNING_GREEN = LV_COLOR_MAKE(0, 200, 83);
static const lv_color_t MOTOR_DISABLED_GRAY = LV_COLOR_MAKE(128, 128, 128);
static const lv_color_t MOTOR_DISABLED_RED = LV_COLOR_MAKE(200, 0, 0);
// Live status label created by the .ino.
// It shows actual RPM, ramp progress, and coating counter.
static lv_obj_t *motorStatusLabel = NULL;

// Target speed input limits
#define TARGET_SPEED_MIN_RPM 2800
#define TARGET_SPEED_MAX_RPM 7500

static lv_obj_t *targetSpeedErrorBox = NULL;

static void targetSpeedErrorDeleteEvent(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE)
    {
        targetSpeedErrorBox = NULL;
    }
}

static void clearTargetSpeedError()
{
    if (targetSpeedErrorBox != NULL)
    {
        lv_obj_del(targetSpeedErrorBox);
        targetSpeedErrorBox = NULL;
    }

    lv_obj_set_style_text_color(
        objects.target_speed_value_text_area,
        lv_color_hex(0x000000),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );
}

static void showTargetSpeedError(const char *message)
{
    clearTargetSpeedError();

    lv_obj_set_style_text_color(
        objects.target_speed_value_text_area,
        lv_color_hex(0xFF0000),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    targetSpeedErrorBox = lv_msgbox_create(
        NULL,
        "RPM Error",
        message,
        NULL,
        true
    );

    lv_obj_add_event_cb(
        targetSpeedErrorBox,
        targetSpeedErrorDeleteEvent,
        LV_EVENT_DELETE,
        NULL
    );

    lv_obj_center(targetSpeedErrorBox);
}

static bool validateTargetSpeedInput(int32_t value)
{
    if (value < TARGET_SPEED_MIN_RPM)
    {
        showTargetSpeedError("Enter RPM 2800 or above");
        return false;
    }

    if (value > TARGET_SPEED_MAX_RPM)
    {
        showTargetSpeedError("Enter RPM 7500 or below");
        return false;
    }

    clearTargetSpeedError();
    return true;
}

// =====================================================
// SPIN COATER PARAMETERS
// =====================================================

int32_t targetSpeed = 5000;
int32_t rampSpeed   = 5;
int32_t coatingTime = 10;

bool coaterRunning = false;

// Runtime counters.
// rampSpeed is treated as ramp time in seconds.
// coatingTime starts after the ramp reaches the target.
volatile uint32_t rampElapsedMs = 0;
volatile uint32_t rampDownElapsedMs = 0;
volatile uint32_t coatingElapsedMs = 0;
volatile bool rampComplete = false;
volatile bool rampDownActive = false;
volatile bool motorAutoStopped = false;

unsigned long motorStartMs = 0;
unsigned long coatingStartMs = 0;
unsigned long rampDownStartMs = 0;
float rampDownStartPwm = ESC_MIN_US;

// =====================================================
// SAVED SETTINGS
// =====================================================

struct SpinSetting
{
    int32_t targetSpeed;
    int32_t rampSpeed;
    int32_t coatingTime;
};

SpinSetting savedSettings[3] =
{
    {5000, 5, 10},
    {5000, 5, 10},
    {5000, 5, 10}
};

// Currently selected setting on MAIN screen
int selectedMainSetting = 0;

// Currently selected option in SAVE PARAMETERS panel
int selectedSaveSetting = 0;

// Temporary values while editing a saved setting
int32_t saveTargetSpeed = 5000;
int32_t saveRampSpeed   = 5;
int32_t saveCoatingTime = 10;

// =====================================================
// EDIT MODE
// =====================================================

enum EditMode
{
    EDIT_MAIN_TARGET,
    EDIT_MAIN_RAMP,
    EDIT_MAIN_TIME,

    EDIT_SAVE_TARGET,
    EDIT_SAVE_RAMP,
    EDIT_SAVE_TIME
};

EditMode currentEditMode = EDIT_MAIN_TARGET;

// =====================================================
// ESP32 NVS STORAGE
// =====================================================

Preferences preferences;

#define SETTINGS_NAMESPACE "spinsettings"

// =====================================================
// TOUCH CONFIGURATION
// =====================================================

#define XP 12
#define XM 32
#define YP 2
#define YM 13

#define MINPRESSURE 200
#define MAXPRESSURE 40000

const int TS_LEFT = -2420;
const int TS_RT   = 555;
const int TS_TOP  = 780;
const int TS_BOT  = -2650;

TouchScreen ts(
    XP,
    YP,
    XM,
    YM,
    0
);

// =====================================================
// DISPLAY
// =====================================================

#define TFT_WIDTH  320
#define TFT_HEIGHT 240

#define BUFFER_LINES 20

static lv_color_t buf1[TFT_WIDTH * BUFFER_LINES];

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// =====================================================
// DISPLAY FLUSH
// =====================================================

void my_disp_flush(
    lv_disp_drv_t *disp,
    const lv_area_t *area,
    lv_color_t *color_p)
{
    uint32_t width =
        area->x2 - area->x1 + 1;

    uint32_t height =
        area->y2 - area->y1 + 1;

    uint32_t size =
        width * height;

    tft.setAddrWindow(
        area->x1,
        area->y1,
        area->x2,
        area->y2
    );

    tft.pushColors(
        (uint16_t *)&color_p->full,
        size,
        true
    );

    lv_disp_flush_ready(disp);
}


// =====================================================
// TOUCH INPUT
// =====================================================

void my_touch_read(
    lv_indev_drv_t *indev,
    lv_indev_data_t *data)
{
    TSPoint p = ts.getPoint();

    // The TouchScreen library temporarily changes the shared
    // TFT pins to read the resistive touch panel.
    // Restore them immediately after the read.
    pinMode(YP, OUTPUT);
    pinMode(XM, OUTPUT);

    digitalWrite(YP, HIGH);
    digitalWrite(XM, HIGH);

    // Avoid unnecessary abs() work when pressure is already valid.
    int16_t pressure = p.z;
    if (pressure < 0)
        pressure = -pressure;

    if (pressure > MINPRESSURE && pressure < MAXPRESSURE)
    {
        int16_t x = map(
            p.y,
            TS_TOP,
            TS_BOT,
            0,
            TFT_WIDTH - 1
        );

        int16_t y = map(
            p.x,
            TS_RT,
            TS_LEFT,
            TFT_HEIGHT - 1,
            0
        );

        x = constrain(x, 0, TFT_WIDTH - 1);
        y = constrain(y, 0, TFT_HEIGHT - 1);

        y += TOUCH_Y_OFFSET;
        y = constrain(y, 0, TFT_HEIGHT - 1);

        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}  
// =====================================================
// NVS STORAGE
// =====================================================

void loadSavedSettings()
{
    preferences.begin(
        SETTINGS_NAMESPACE,
        true
    );

    for (int i = 0; i < 3; i++)
    {
        char key[20];

        // Target speed
        snprintf(
            key,
            sizeof(key),
            "s%d_target",
            i
        );

        savedSettings[i].targetSpeed =
            preferences.getInt(
                key,
                5000
            );

        // Ramp speed
        snprintf(
            key,
            sizeof(key),
            "s%d_ramp",
            i
        );

        savedSettings[i].rampSpeed =
            preferences.getInt(
                key,
                5
            );

        // Coating time
        snprintf(
            key,
            sizeof(key),
            "s%d_time",
            i
        );

        savedSettings[i].coatingTime =
            preferences.getInt(
                key,
                10
            );
    }

    selectedMainSetting =
        preferences.getInt(
            "selected",
            0
        );

    preferences.end();

    if (selectedMainSetting < 0 ||
        selectedMainSetting > 2)
    {
        selectedMainSetting = 0;
    }

    Serial.println();
    Serial.println("Saved settings loaded:");

    for (int i = 0; i < 3; i++)
    {
        Serial.print("Setting ");
        Serial.println(i + 1);

        Serial.print("  Target = ");
        Serial.println(
            savedSettings[i].targetSpeed
        );

        Serial.print("  Ramp = ");
        Serial.println(
            savedSettings[i].rampSpeed
        );

        Serial.print("  Time = ");
        Serial.println(
            savedSettings[i].coatingTime
        );
    }
}

void saveSettingToFlash(int index)
{
    if (index < 0 || index > 2)
        return;

    preferences.begin(
        SETTINGS_NAMESPACE,
        false
    );

    char key[20];

    // Target speed
    snprintf(
        key,
        sizeof(key),
        "s%d_target",
        index
    );

    preferences.putInt(
        key,
        savedSettings[index].targetSpeed
    );

    // Ramp speed
    snprintf(
        key,
        sizeof(key),
        "s%d_ramp",
        index
    );

    preferences.putInt(
        key,
        savedSettings[index].rampSpeed
    );

    // Coating time
    snprintf(
        key,
        sizeof(key),
        "s%d_time",
        index
    );

    preferences.putInt(
        key,
        savedSettings[index].coatingTime
    );

    // Remember selected setting
    preferences.putInt(
        "selected",
        selectedMainSetting
    );

    preferences.end();

    Serial.print("Setting ");
    Serial.print(index + 1);
    Serial.println(" saved to flash.");
}

// =====================================================
// LABEL UPDATE HELPERS
// =====================================================

void updateMainLabels()
{
    char text[20];

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)targetSpeed
    );

    lv_label_set_text(
        objects.target_speed_value_lable,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)rampSpeed
    );

    lv_label_set_text(
        objects.ramp_speed_value_lable,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)coatingTime
    );

    lv_label_set_text(
        objects.coating_time_value_lable,
        text
    );
}

void updateSaveLabels()
{
    char text[20];

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)saveTargetSpeed
    );

    lv_label_set_text(
        objects.save_target_speed_value_lable,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)saveRampSpeed
    );

    lv_label_set_text(
        objects.save_ramp_speed_value_lable,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "%ld",
        (long)saveCoatingTime
    );

    lv_label_set_text(
        objects.save_coating_time_value_lable,
        text
    );
}

// =====================================================
// LOAD SELECTED SETTING INTO MAIN PARAMETERS
// =====================================================

void loadSelectedSettingToMain()
{
    if (selectedMainSetting < 0 ||
        selectedMainSetting > 2)
    {
        selectedMainSetting = 0;
    }

    targetSpeed =
        savedSettings[selectedMainSetting].targetSpeed;

    rampSpeed =
        savedSettings[selectedMainSetting].rampSpeed;

    coatingTime =
        savedSettings[selectedMainSetting].coatingTime;

    updateMainLabels();

    Serial.println();
    Serial.print("Main Setting ");
    Serial.print(selectedMainSetting + 1);
    Serial.println(" selected.");

    Serial.print("Target Speed = ");
    Serial.println(targetSpeed);

    Serial.print("Ramp Speed = ");
    Serial.println(rampSpeed);

    Serial.print("Coating Time = ");
    Serial.println(coatingTime);
}

// =====================================================
// PANEL HELPERS
// =====================================================

static void show_main_panel()
{
    // Hide splash and About panels before showing Main.
    lv_obj_add_flag(
        objects.splash_screen,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.about_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_clear_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    updateMainLabels();

    lv_group_focus_obj(
        objects.main_panel
    );
}

// =====================================================
// SHOW SAVE PARAMETERS PANEL
// =====================================================

void show_save_parameters_panel(bool loadSavedValues = true)
{
    // Load selected setting only when requested.
    // When returning from a keypad, keep the values
    // that the user just entered.
    if (loadSavedValues)
    {
        saveTargetSpeed =
            savedSettings[selectedSaveSetting].targetSpeed;

        saveRampSpeed =
            savedSettings[selectedSaveSetting].rampSpeed;

        saveCoatingTime =
            savedSettings[selectedSaveSetting].coatingTime;
    }

    updateSaveLabels();

    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_clear_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_group_focus_obj(
        objects.parameter_save_setting_panel
    );

    Serial.print("Save Parameters panel opened. Setting ");
    Serial.println(selectedSaveSetting + 1);
}

// =====================================================
// TARGET SPEED PANEL
// =====================================================

static void show_target_speed_panel()
{
    char text[16];

    if (currentEditMode == EDIT_SAVE_TARGET)
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)saveTargetSpeed
        );
    }
    else
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)targetSpeed
        );
    }

    clearTargetSpeedError();

    lv_textarea_set_text(
        objects.target_speed_value_text_area,
        text
    );

    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_clear_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_textarea_set_cursor_pos(
        objects.target_speed_value_text_area,
        LV_TEXTAREA_CURSOR_LAST
    );

    lv_group_focus_obj(
        objects.target_speed_value_text_area
    );
}

// =====================================================
// RAMP SPEED PANEL
// =====================================================

static void show_ramp_speed_panel()
{
    char text[16];

    if (currentEditMode == EDIT_SAVE_RAMP)
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)saveRampSpeed
        );
    }
    else
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)rampSpeed
        );
    }

    lv_textarea_set_text(
        objects.ramp_speed_value_text_area,
        text
    );

    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_clear_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_textarea_set_cursor_pos(
        objects.ramp_speed_value_text_area,
        LV_TEXTAREA_CURSOR_LAST
    );

    lv_group_focus_obj(
        objects.ramp_speed_value_text_area
    );
}

// =====================================================
// COATING TIME PANEL
// =====================================================

static void show_coating_time_panel()
{
    char text[16];

    if (currentEditMode == EDIT_SAVE_TIME)
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)saveCoatingTime
        );
    }
    else
    {
        snprintf(
            text,
            sizeof(text),
            "%ld",
            (long)coatingTime
        );
    }

    lv_textarea_set_text(
        objects.counting_time_value_text_area,
        text
    );

    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_clear_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_textarea_set_cursor_pos(
        objects.counting_time_value_text_area,
        LV_TEXTAREA_CURSOR_LAST
    );

    lv_group_focus_obj(
        objects.counting_time_value_text_area
    );
}

// =====================================================
// MAIN PANEL BUTTON EVENTS
// =====================================================

static void target_speed_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_MAIN_TARGET;

        Serial.println(
            "Target speed selected"
        );

        show_target_speed_panel();
    }
}

static void ramp_speed_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_MAIN_RAMP;

        Serial.println(
            "Ramp speed selected"
        );

        show_ramp_speed_panel();
    }
}

static void coating_time_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_MAIN_TIME;

        Serial.println(
            "Coating time selected"
        );

        show_coating_time_panel();
    }
}

// =====================================================
// SETTINGS BUTTON
// =====================================================

static void settings_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    // Start with the same setting that is selected
    // on the main screen.
    selectedSaveSetting =
        selectedMainSetting;

    if (selectedSaveSetting < 0 ||
        selectedSaveSetting > 2)
    {
        selectedSaveSetting = 0;
    }

    // Keep the save-panel dropdown synchronized.
    lv_dropdown_set_selected(
        objects.save_option_dropdown,
        selectedSaveSetting
    );

    // Load the selected setting into the save panel.
    show_save_parameters_panel(true);

    Serial.print(
        "Settings button clicked. Editing Setting "
    );

    Serial.println(
        selectedSaveSetting + 1
    );
}

// =====================================================
// SAVE PANEL BUTTON EVENTS
// =====================================================

static void save_target_speed_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_SAVE_TARGET;

        Serial.println(
            "Save Target Speed selected"
        );

        show_target_speed_panel();
    }
}

static void save_ramp_speed_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_SAVE_RAMP;

        Serial.println(
            "Save Ramp Speed selected"
        );

        show_ramp_speed_panel();
    }
}

static void save_coating_time_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        currentEditMode =
            EDIT_SAVE_TIME;

        Serial.println(
            "Save Coating Time selected"
        );

        show_coating_time_panel();
    }
}

// =====================================================
// TARGET SPEED KEYPAD
// =====================================================

static void target_speed_keypad_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY)
        return;

    const char *text =
        lv_textarea_get_text(
            objects.target_speed_value_text_area
        );

    int32_t value = atol(text);

    if (value < 0)
        value = 0;

    // Target RPM must be between 2800 and 8000 inclusive.
    if (!validateTargetSpeedInput(value))
    {
        Serial.print("Invalid target RPM: ");
        Serial.println(value);
        return;
    }

    // -----------------------------------------------
    // MAIN PANEL EDIT
    // -----------------------------------------------

    if (currentEditMode == EDIT_MAIN_TARGET)
    {
        targetSpeed = value;

        Serial.print(
            "Target speed = "
        );

        Serial.println(
            targetSpeed
        );

        show_main_panel();
    }

    // -----------------------------------------------
    // SAVE PANEL EDIT
    // -----------------------------------------------

    else if (currentEditMode == EDIT_SAVE_TARGET)
    {
        saveTargetSpeed = value;

        Serial.print(
            "Save Target speed = "
        );

        Serial.println(
            saveTargetSpeed
        );

        updateSaveLabels();

        show_save_parameters_panel(false);
    }
}

// =====================================================
// RAMP SPEED KEYPAD
// =====================================================

static void ramp_speed_keypad_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY)
        return;

    const char *text =
        lv_textarea_get_text(
            objects.ramp_speed_value_text_area
        );

    int32_t value =
        atol(text);

    if (value < 0)
        value = 0;

    // -----------------------------------------------
    // MAIN PANEL EDIT
    // -----------------------------------------------

    if (currentEditMode == EDIT_MAIN_RAMP)
    {
        rampSpeed = value;

        Serial.print(
            "Ramp speed = "
        );

        Serial.println(
            rampSpeed
        );

        show_main_panel();
    }

    // -----------------------------------------------
    // SAVE PANEL EDIT
    // -----------------------------------------------

    else if (currentEditMode == EDIT_SAVE_RAMP)
    {
        saveRampSpeed = value;

        Serial.print(
            "Save Ramp speed = "
        );

        Serial.println(
            saveRampSpeed
        );

        updateSaveLabels();

        show_save_parameters_panel(false);
    }
}

// =====================================================
// COATING TIME KEYPAD
// =====================================================

static void coating_time_keypad_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY)
        return;

    const char *text =
        lv_textarea_get_text(
            objects.counting_time_value_text_area
        );

    int32_t value =
        atol(text);

    if (value < 0)
        value = 0;

    // -----------------------------------------------
    // MAIN PANEL EDIT
    // -----------------------------------------------

    if (currentEditMode == EDIT_MAIN_TIME)
    {
        coatingTime = value;

        Serial.print(
            "Coating time = "
        );

        Serial.println(
            coatingTime
        );

        show_main_panel();
    }

    // -----------------------------------------------
    // SAVE PANEL EDIT
    // -----------------------------------------------

    else if (currentEditMode == EDIT_SAVE_TIME)
    {
        saveCoatingTime = value;

        Serial.print(
            "Save Coating time = "
        );

        Serial.println(
            saveCoatingTime
        );

        updateSaveLabels();

        show_save_parameters_panel(false);
    }
}

// =====================================================
// SAVE OPTION DROPDOWN
// =====================================================

static void save_option_dropdown_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;

    selectedSaveSetting =
        lv_dropdown_get_selected(
            objects.save_option_dropdown
        );

    if (selectedSaveSetting < 0 ||
        selectedSaveSetting > 2)
    {
        selectedSaveSetting = 0;
    }

    Serial.print(
        "Save option selected: Setting "
    );

    Serial.println(
        selectedSaveSetting + 1
    );

    // Load that setting's existing values
    saveTargetSpeed =
        savedSettings[selectedSaveSetting].targetSpeed;

    saveRampSpeed =
        savedSettings[selectedSaveSetting].rampSpeed;

    saveCoatingTime =
        savedSettings[selectedSaveSetting].coatingTime;

    updateSaveLabels();
}

// =====================================================
// SAVE BUTTON
// =====================================================

static void save_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    // Store temporary values into selected setting
    savedSettings[selectedSaveSetting].targetSpeed =
        saveTargetSpeed;

    savedSettings[selectedSaveSetting].rampSpeed =
        saveRampSpeed;

    savedSettings[selectedSaveSetting].coatingTime =
        saveCoatingTime;

    // Save to ESP32 flash
    saveSettingToFlash(
        selectedSaveSetting
    );

    Serial.println();
    Serial.println("========================");
    Serial.print("SETTING ");
    Serial.print(selectedSaveSetting + 1);
    Serial.println(" SAVED");
    Serial.print("Target Speed: ");
    Serial.println(saveTargetSpeed);
    Serial.print("Ramp Speed: ");
    Serial.println(saveRampSpeed);
    Serial.print("Coating Time: ");
    Serial.println(saveCoatingTime);
    Serial.println("========================");

    // After saving, make this setting the active
    // setting on the main screen.
    selectedMainSetting =
        selectedSaveSetting;

    targetSpeed =
        saveTargetSpeed;

    rampSpeed =
        saveRampSpeed;

    coatingTime =
        saveCoatingTime;

    // Update main dropdown
    lv_dropdown_set_selected(
        objects.main_panel_select_option_save_dropdown,
        selectedMainSetting
    );

    updateMainLabels();

    // Go back to main
    show_main_panel();
}

// =====================================================
// MAIN SETTINGS DROPDOWN
// =====================================================

static void main_settings_dropdown_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;

    selectedMainSetting =
        lv_dropdown_get_selected(
            objects.main_panel_select_option_save_dropdown
        );

    if (selectedMainSetting < 0 ||
        selectedMainSetting > 2)
    {
        selectedMainSetting = 0;
    }

    Serial.print(
        "Main dropdown selected: Setting "
    );

    Serial.println(
        selectedMainSetting + 1
    );

    // Load saved parameters
    loadSelectedSettingToMain();
}

// =====================================================
// MOTOR RUNNING UI COLORS
// =====================================================
static void saveNormalButtonColors()
{
    if (normalButtonColorsSaved) return;
    normalTargetButtonColor = lv_obj_get_style_bg_color(objects.target_speed_value_enter_button, LV_PART_MAIN);
    normalRampButtonColor = lv_obj_get_style_bg_color(objects.ramp_speed_value_enter_button, LV_PART_MAIN);
    normalCoatingButtonColor = lv_obj_get_style_bg_color(objects.coating_time_value_enter_button, LV_PART_MAIN);
    normalStartButtonColor = lv_obj_get_style_bg_color(objects.start_button, LV_PART_MAIN);
    normalStopButtonColor = lv_obj_get_style_bg_color(objects.stop_button, LV_PART_MAIN);
    normalButtonColorsSaved = true;
}

static void setButtonColor(lv_obj_t *button, lv_color_t color)
{
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
}

static void setMotorRunningUI()
{
    saveNormalButtonColors();
    setButtonColor(objects.target_speed_value_enter_button, MOTOR_RUNNING_GREEN);
    setButtonColor(objects.ramp_speed_value_enter_button, MOTOR_RUNNING_GREEN);
    setButtonColor(objects.coating_time_value_enter_button, MOTOR_RUNNING_GREEN);
    setButtonColor(objects.start_button, MOTOR_DISABLED_GRAY);
    setButtonColor(objects.stop_button, MOTOR_DISABLED_RED);
}

static void restoreNormalUI()
{
    if (!normalButtonColorsSaved) return;
    setButtonColor(objects.target_speed_value_enter_button, normalTargetButtonColor);
    setButtonColor(objects.ramp_speed_value_enter_button, normalRampButtonColor);
    setButtonColor(objects.coating_time_value_enter_button, normalCoatingButtonColor);
    setButtonColor(objects.start_button, normalStartButtonColor);
    setButtonColor(objects.stop_button, normalStopButtonColor);
}

// =====================================================
// HALL SENSOR INTERRUPT
// =====================================================

void IRAM_ATTR hallISR()
{
    pulseCount++;
}

// =====================================================
// ESC PWM
// =====================================================

static void setESC(float microseconds)
{
    microseconds = constrain(
        microseconds,
        (float)ESC_MIN_US,
        (float)ESC_MAX_US
    );

    uint32_t duty =
        ((uint64_t)microseconds * 65535UL) / 20000UL;

    ledcWrite(ESC_PIN, duty);
}

// =====================================================
// RPM CALCULATION
// =====================================================

static void calculateRPM()
{
    unsigned long now = millis();

    if ((now - lastRPMTime) < RPM_INTERVAL_MS)
        return;

    unsigned long elapsed = now - lastRPMTime;
    lastRPMTime = now;

    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    if (pulses > 0)
    {
        float newRPM =
            ((float)pulses * 60000.0f) /
            ((float)PULSES_PER_REV * (float)elapsed);

        rpm = newRPM;

        filteredRPM =
            filteredRPM +
            RPM_FILTER * (rpm - filteredRPM);
    }
    else
    {
        filteredRPM *= 0.70f;

        if (filteredRPM < 10.0f)
            filteredRPM = 0.0f;
    }
}

// =====================================================
// FEEDFORWARD PWM
// =====================================================

static float estimatePWM(float rpmTarget)
{
    if (rpmTarget <= 0)
        return ESC_MIN_US;

    if (rpmTarget < 2000)
        return 1225.0f;

    if (rpmTarget < 3500)
    {
        return 1235.0f +
               (rpmTarget - 3000.0f) *
               (11.0f / 500.0f);
    }

    if (rpmTarget < 4800)
    {
        return 1246.0f +
               (rpmTarget - 3500.0f) *
               (44.0f / 1300.0f);
    }

    if (rpmTarget < 5000)
    {
        return 1290.0f +
               (rpmTarget - 4800.0f) *
               (5.0f / 200.0f);
    }

    return 1295.0f +
           (rpmTarget - 5000.0f) * 0.04f;
}

// =====================================================
// PWM RAMP HELPER
// =====================================================
//
// A shorter ramp time produces a larger PWM step.
// A longer ramp time produces a smaller PWM step.
// The step is calculated from the PWM range and the
// requested ramp time.

static float calculateRampPwmStep(float startPwm, float endPwm)
{
    if (rampSpeed <= 0)
        return fabsf(endPwm - startPwm);

    float totalSteps =
        ((float)rampSpeed * 1000.0f) /
        (float)CONTROL_INTERVAL_MS;

    if (totalSteps < 1.0f)
        totalSteps = 1.0f;

    float step =
        fabsf(endPwm - startPwm) / totalSteps;

    if (step < 0.01f)
        step = 0.01f;

    return step;
}

// =====================================================
// AUTO STOP HELPER
// =====================================================

static void finishAutoStop()
{
    coaterRunning = false;
    rampDownActive = false;

    integral = 0.0f;
    error = 0.0f;
    pwmUs = ESC_MIN_US;
    setESC(ESC_MIN_US);

    rampDownElapsedMs = 0;
    rampComplete = false;
    coatingStartMs = 0;
    rampDownStartMs = 0;
    rampDownStartPwm = ESC_MIN_US;
    motorAutoStopped = true;
}

// =====================================================
// PI CONTROL
// =====================================================

static void updatePI()
{
    unsigned long now = millis();

    if ((now - lastControlTime) < CONTROL_INTERVAL_MS)
        return;

    float dt =
        (now - lastControlTime) / 1000.0f;

    lastControlTime = now;

    if (!coaterRunning)
    {
        integral = 0.0f;
        error = 0.0f;
        pwmUs = ESC_MIN_US;
        setESC(pwmUs);
        return;
    }

    // =================================================
    // RAMP DOWN
    // =================================================
    // After coating time completes, decrease PWM using
    // the same ramp-time based step logic as ramp-up.
    if (rampDownActive)
    {
        uint32_t downElapsed =
            now - rampDownStartMs;

        rampDownElapsedMs = downElapsed;

        // 0 ramp time means no ramp-down. Stop immediately.
        if (rampSpeed <= 0)
        {
            pwmUs = ESC_MIN_US;
            setESC(ESC_MIN_US);
            finishAutoStop();
            Serial.println("Ramp-down disabled. Motor stopped.");
            return;
        }

        uint32_t rampTotalMs =
            (uint32_t)rampSpeed * 1000UL;

        // Ramp-down is complete.
        if (downElapsed >= rampTotalMs ||
            pwmUs <= ESC_MIN_US)
        {
            pwmUs = ESC_MIN_US;
            setESC(ESC_MIN_US);
            rampDownElapsedMs = rampTotalMs;
            finishAutoStop();
            Serial.println("Ramp-down completed. Motor stopped.");
            return;
        }

        float rampDownStep =
            calculateRampPwmStep(
                rampDownStartPwm,
                ESC_MIN_US
            );

        pwmUs -= rampDownStep;

        if (pwmUs < ESC_MIN_US)
            pwmUs = ESC_MIN_US;

        setESC(pwmUs);

        // Coating timer stays at its completed value during
        // ramp-down. The ramp label shows the ramp-down timer.
        coatingElapsedMs =
            (uint32_t)coatingTime * 1000UL;

        return;
    }

    // =================================================
    // RAMP UP / SPEED CONTROL
    // =================================================
    float controlTarget = (float)targetSpeed;

    if (controlTarget <= 0.0f)
    {
        integral = 0.0f;
        pwmUs = ESC_MIN_US;
        setESC(pwmUs);
        return;
    }

    error = controlTarget - filteredRPM;

    float basePWM = estimatePWM(controlTarget);

    integral += error * dt;

    integral = constrain(
        integral,
        -500.0f,
        500.0f
    );

    float correction =
        (Kp * error) +
        (Ki * integral);

    float desiredPWM =
        basePWM + correction;

    desiredPWM = constrain(
        desiredPWM,
        (float)ESC_MIN_US,
        (float)ESC_MAX_US
    );

    // Ramp time controls the PWM step only while the
    // motor is still approaching the target RPM.
    if (!rampComplete)
    {
        if (rampSpeed <= 0)
        {
            // No ramp time means maximum acceleration from
            // the controller. Jump directly to required PWM.
            pwmUs = desiredPWM;
        }
        else
        {
            float rampUpStep =
                calculateRampPwmStep(
                    ESC_MIN_US,
                    basePWM
                );

            float pwmDifference =
                desiredPWM - pwmUs;

            if (pwmDifference > rampUpStep)
                pwmDifference = rampUpStep;

            if (pwmDifference < -rampUpStep)
                pwmDifference = -rampUpStep;

            pwmUs += pwmDifference;
        }
    }
    else
    {
        // Once target RPM is reached, use a fixed PWM step
        // for normal PI speed correction.
        float pwmDifference =
            desiredPWM - pwmUs;

        if (pwmDifference > FIXED_PWM_STEP)
            pwmDifference = FIXED_PWM_STEP;

        if (pwmDifference < -FIXED_PWM_STEP)
            pwmDifference = -FIXED_PWM_STEP;

        pwmUs += pwmDifference;
    }

    pwmUs = constrain(
        pwmUs,
        (float)ESC_MIN_US,
        (float)ESC_MAX_US
    );

    setESC(pwmUs);

    // Ramp time shown in the UI is the actual time from
    // START until measured RPM reaches target within +/-100 RPM.
    if (!rampComplete)
    {
        rampElapsedMs = now - motorStartMs;

        float targetRPM = (float)targetSpeed;

        if ((filteredRPM - targetRPM) < 100.0f &&
            (targetRPM - 100.0f) < filteredRPM)
        {
            // Do NOT modify filteredRPM here. The requested
            // +/-100 snap is only for UI display.
            rampComplete = true;
            rampElapsedMs = now - motorStartMs;
            coatingStartMs = now;
            coatingElapsedMs = 0;

            Serial.println(
                "Target RPM reached. Coating timer started."
            );
        }
    }

    if (rampComplete)
    {
        coatingElapsedMs = now - coatingStartMs;
    }
    else
    {
        coatingElapsedMs = 0;
    }

    // =================================================
    // COATING COMPLETE -> START RAMP DOWN
    // =================================================
    if (coatingTime > 0 &&
        rampComplete &&
        coatingElapsedMs >= ((uint32_t)coatingTime * 1000UL))
    {
        rampDownActive = true;
        rampDownStartMs = now;
        rampDownElapsedMs = 0;
        rampDownStartPwm = pwmUs;

        coatingElapsedMs =
            (uint32_t)coatingTime * 1000UL;

        integral = 0.0f;
        error = 0.0f;

        // For zero ramp time, stop immediately on this same cycle.
        if (rampSpeed <= 0)
        {
            pwmUs = ESC_MIN_US;
            setESC(ESC_MIN_US);
            finishAutoStop();
            Serial.println("Coating complete. No ramp-down. Motor stopped.");
            return;
        }

        Serial.println(
            "Coating time completed. Ramp-down started."
        );
    }
}

// =====================================================
// MOTOR CONTROL TASK
// =====================================================
//
// This task never waits for UART and never performs LVGL work.
// It runs every 10 ms and each operation is short.
// The Hall ISR only increments a counter.

static void motorTask(void *parameter)
{
    TickType_t lastWake =
        xTaskGetTickCount();

    for (;;)
    {
        calculateRPM();
        updatePI();

        vTaskDelayUntil(
            &lastWake,
            pdMS_TO_TICKS(10)
        );
    }
}

// =====================================================
// FREERTOS LVGL/UI TASK
// =====================================================
//
// LVGL stays on one task only.
// Motor calculations run on the other ESP32 core.

static void updateMotorUI()
{
    // While stopped, keep the saved NVS values displayed
    // on the main screen. STOP/auto-stop restores them.
    if (!coaterRunning)
        return;

    char rpmText[16];
    char rampText[16];
    char coatText[16];

    int32_t safeRamp = rampSpeed;
    int32_t safeCoating = coatingTime;

    if (safeRamp < 0)
        safeRamp = 0;

    if (safeCoating < 0)
        safeCoating = 0;

    uint32_t rampSec;

    // Ramp-up timer before target is reached.
    // Ramp-down timer after coating is complete.
    if (rampDownActive)
    {
        rampSec = rampDownElapsedMs / 1000UL;
    }
    else
    {
        rampSec = rampElapsedMs / 1000UL;
    }

    uint32_t coatSec = coatingElapsedMs / 1000UL;

    if (rampSec > (uint32_t)safeRamp)
        rampSec = (uint32_t)safeRamp;

    if (coatSec > (uint32_t)safeCoating)
        coatSec = (uint32_t)safeCoating;

    // Live RPM while running.
    // Requested display snap: if RPM is within +/-100 of target,
    // show the exact target RPM on the UI.
    float displayRPM = filteredRPM;
    float targetRPM = (float)targetSpeed;

    if ((displayRPM - targetRPM) < 100.0f &&
        (targetRPM - 100.0f) < displayRPM)
    {
        displayRPM = targetRPM;
    }

    snprintf(
        rpmText,
        sizeof(rpmText),
        "%.0f",
        displayRPM
    );

    // Ramp-up: elapsed / total ramp time
    // Ramp-down: elapsed / total ramp-down time
    snprintf(
        rampText,
        sizeof(rampText),
        "%lu/%ld",
        (unsigned long)rampSec,
        (long)safeRamp
    );

    // Coating timer starts only after target RPM is reached.
    snprintf(
        coatText,
        sizeof(coatText),
        "%lu/%ld",
        (unsigned long)coatSec,
        (long)safeCoating
    );

    lv_label_set_text(
        objects.target_speed_value_lable,
        rpmText
    );

    lv_label_set_text(
        objects.ramp_speed_value_lable,
        rampText
    );

    lv_label_set_text(
        objects.coating_time_value_lable,
        coatText
    );
}

static void createMotorStatusLabel()
{
    motorStatusLabel =
        lv_label_create(objects.main_panel);

    lv_obj_set_pos(
        motorStatusLabel,
        5,
        198
    );

    lv_obj_set_size(
        motorStatusLabel,
        215,
        40
    );

    lv_obj_set_style_text_font(
        motorStatusLabel,
        &lv_font_montserrat_14,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_label_set_text(
        motorStatusLabel,
        "RPM: 0\\nRamp: 0/0 s  Coat: 0/0 s"
    );
}

static void uiTask(void *parameter)
{
    TickType_t lastWake =
        xTaskGetTickCount();

    for (;;)
    {
        check_splash_screen();

        if (motorAutoStopped)
        {
            motorAutoStopped = false;
            loadSelectedSettingToMain();
            restoreNormalUI();
        }

        updateMotorUI();

        ui_tick();

        lv_timer_handler();

        // Keep this task responsive.
        vTaskDelayUntil(
            &lastWake,
            pdMS_TO_TICKS(5)
        );
    }
}

// =====================================================
// START / STOP
// =====================================================

static void start_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    if (coaterRunning)
        return;

    // Snapshot the current UI settings.
    // Motor control uses these values locally.
    if (targetSpeed <= 0)
    {
        Serial.println("Invalid target speed.");
        return;
    }

    if (rampSpeed < 0)
        rampSpeed = 0;

    if (coatingTime < 0)
        coatingTime = 0;

    noInterrupts();
    pulseCount = 0;
    interrupts();

    rpm = 0.0f;
    filteredRPM = 0.0f;
    error = 0.0f;
    integral = 0.0f;

    pwmUs = ESC_MIN_US;
    setESC(ESC_MIN_US);

    motorStartMs = millis();
    coatingStartMs = 0;
    lastRPMTime = motorStartMs;
    lastControlTime = motorStartMs;

    rampElapsedMs = 0;
    rampDownElapsedMs = 0;
    coatingElapsedMs = 0;
    rampComplete = false;
    rampDownActive = false;
    motorAutoStopped = false;
    rampDownStartMs = 0;
    rampDownStartPwm = ESC_MIN_US;

    coaterRunning = true;

    setMotorRunningUI();

    // Show RPM/ramp/coating counters immediately on Start.
    updateMotorUI();

    Serial.println();
    Serial.println("==============================");
    Serial.println("LOCAL MOTOR START");
    Serial.print("Target RPM: ");
    Serial.println(targetSpeed);
    Serial.print("Ramp Time: ");
    Serial.print(rampSpeed);
    Serial.println(" s");
    Serial.print("Coating Time: ");
    Serial.print(coatingTime);
    Serial.println(" s");
    Serial.println("==============================");
}

static void stop_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    coaterRunning = false;

    integral = 0.0f;
    error = 0.0f;
    pwmUs = ESC_MIN_US;

    setESC(ESC_MIN_US);

    noInterrupts();
    pulseCount = 0;
    interrupts();

    rpm = 0.0f;
    filteredRPM = 0.0f;

    rampElapsedMs = 0;
    rampDownElapsedMs = 0;
    coatingElapsedMs = 0;
    rampComplete = false;
    rampDownActive = false;
    coatingStartMs = 0;
    rampDownStartMs = 0;
    rampDownStartPwm = ESC_MIN_US;
    motorAutoStopped = false;

    // Restore the NVS setting values on the main screen.
    loadSelectedSettingToMain();

    restoreNormalUI();

    Serial.println("LOCAL MOTOR STOP");
}

// =====================================================
// SPLASH SCREEN FUNCTIONS
// =====================================================

static void show_splash_screen()
{
    // Show splash screen
    lv_obj_clear_flag(
        objects.splash_screen,
        LV_OBJ_FLAG_HIDDEN
    );

    // Hide all other panels
    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    // IMPORTANT: hide About panel during splash
    lv_obj_add_flag(
        objects.about_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    splashStartTime = millis();
    splashActive = true;

    Serial.println("Splash screen started");
}


// =====================================================
// CHECK SPLASH SCREEN TIME
// =====================================================

static void check_splash_screen()
{
    if (splashActive)
    {
        if (millis() - splashStartTime >= SPLASH_TIME)
        {
            // Hide splash
            lv_obj_add_flag(
                objects.splash_screen,
                LV_OBJ_FLAG_HIDDEN
            );

            // Show main screen
            show_main_panel();

            splashActive = false;

            Serial.println("Splash screen finished");
            Serial.println("Main UI started");
        }
    }
}

// =====================================================
// ABOUT PANEL
// =====================================================

// Forward declaration
static void show_about_panel();

static void about_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        show_about_panel();
    }
}

static void back_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        show_main_panel();
    }
}

static void show_about_panel()
{
    // Hide splash
    lv_obj_add_flag(
        objects.splash_screen,
        LV_OBJ_FLAG_HIDDEN
    );

    // Hide Main
    lv_obj_add_flag(
        objects.main_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    // Hide other panels
    lv_obj_add_flag(
        objects.target_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.ramp_speed_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.coating_time_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_add_flag(
        objects.parameter_save_setting_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    // Show About
    lv_obj_clear_flag(
        objects.about_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    Serial.println("About panel opened");
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);

    // =================================================
    // LOCAL MOTOR HARDWARE
    // =================================================

    pinMode(HALL_PIN, INPUT_PULLUP);

    attachInterrupt(
        digitalPinToInterrupt(HALL_PIN),
        hallISR,
        RISING
    );

    bool pwmOK = ledcAttach(
        ESC_PIN,
        ESC_FREQ,
        ESC_RESOLUTION
    );

    if (!pwmOK)
    {
        Serial.println("ERROR: LEDC attach failed!");
        while (true)
        {
            delay(1000);
        }
    }

    pwmUs = ESC_MIN_US;
    setESC(ESC_MIN_US);


    Serial.println();
    Serial.println(
        "Starting TFT + LVGL + EEZ Studio"
    );

    // =================================================
    // LOAD SAVED SETTINGS
    // =================================================

    loadSavedSettings();

    // =================================================
    // TFT INITIALIZATION
    // =================================================

    uint16_t ID = tft.readID();

    Serial.print(
        "TFT ID = 0x"
    );

    Serial.println(
        ID,
        HEX
    );

    if (ID == 0xD3D3)
    {
        ID = 0x9486;
    }

    tft.begin(ID);

    tft.setRotation(3);

    tft.fillScreen(0x0000);

    Serial.println(
        "TFT initialized"
    );

    // =================================================
    // LVGL INITIALIZATION
    // =================================================

    lv_init();

    Serial.println(
        "LVGL initialized"
    );

    // =================================================
    // DRAW BUFFER
    // =================================================

    lv_disp_draw_buf_init(
        &draw_buf,
        buf1,
        NULL,
        TFT_WIDTH * BUFFER_LINES
    );

    // =================================================
    // DISPLAY DRIVER
    // =================================================

    lv_disp_drv_init(
        &disp_drv
    );

    disp_drv.hor_res =
        TFT_WIDTH;

    disp_drv.ver_res =
        TFT_HEIGHT;

    disp_drv.flush_cb =
        my_disp_flush;

    disp_drv.draw_buf =
        &draw_buf;

    lv_disp_drv_register(
        &disp_drv
    );

    Serial.println(
        "Display driver initialized"
    );

    // =================================================
    // TOUCH DRIVER
    // =================================================

    lv_indev_drv_init(
        &indev_drv
    );

    indev_drv.type =
        LV_INDEV_TYPE_POINTER;

    indev_drv.read_cb =
        my_touch_read;

    // Poll touch frequently so rapid consecutive taps are detected.


    lv_indev_drv_register(
        &indev_drv
    );

    Serial.println(
        "Touch driver initialized"
    );

    // =================================================
    // EEZ STUDIO UI
    // =================================================

    ui_init();

   // createMotorStatusLabel();

    lv_obj_add_event_cb(
    objects.about_button,
    about_button_event,
    LV_EVENT_CLICKED,
    NULL
);

lv_obj_add_event_cb(
    objects.back_button,
    back_button_event,
    LV_EVENT_CLICKED,
    NULL
);

    lv_obj_set_style_bg_color(objects.stop_button,lv_color_hex(0x808080),LV_PART_MAIN);

    Serial.println(
        "EEZ Studio UI initialized"
    );

    // Save the original EEZ Studio button colors.
    saveNormalButtonColors();

    // Make sure About is hidden at startup.
    lv_obj_add_flag(
        objects.about_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    // =================================================
    // MAIN PARAMETER BUTTONS
    // =================================================

    lv_obj_add_event_cb(
        objects.target_speed_value_enter_button,
        target_speed_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        objects.ramp_speed_value_enter_button,
        ramp_speed_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        objects.coating_time_value_enter_button,
        coating_time_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    // =================================================
    // SETTINGS BUTTON
    // =================================================

    lv_obj_add_event_cb(
        objects.settings_button,
        settings_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    // =================================================
    // SAVE PARAMETER BUTTONS
    // =================================================

    lv_obj_add_event_cb(
        objects.save_target_speed_value_enter_button,
        save_target_speed_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        objects.save_ramp_speed_value_enter_button,
        save_ramp_speed_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        objects.save_coating_time_value_enter_button,
        save_coating_time_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    // =================================================
    // KEYPAD READY EVENTS
    // =================================================

    lv_obj_add_event_cb(
        objects.target_speed_keypad,
        target_speed_keypad_event,
        LV_EVENT_READY,
        NULL
    );

    lv_obj_add_event_cb(
        objects.ramp_speed_keypad,
        ramp_speed_keypad_event,
        LV_EVENT_READY,
        NULL
    );

    lv_obj_add_event_cb(
        objects.coating_time_keypad,
        coating_time_keypad_event,
        LV_EVENT_READY,
        NULL
    );

    // =================================================
    // SAVE DROPDOWN
    // =================================================

    lv_obj_add_event_cb(
        objects.save_option_dropdown,
        save_option_dropdown_event,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );

    // =================================================
    // SAVE BUTTON
    // =================================================

    lv_obj_add_event_cb(
        objects.save_button,
        save_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    // =================================================
    // MAIN SETTINGS DROPDOWN
    // =================================================

    lv_obj_add_event_cb(
        objects.main_panel_select_option_save_dropdown,
        main_settings_dropdown_event,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );

    // =================================================
    // START / STOP
    // =================================================

    lv_obj_add_event_cb(
        objects.start_button,
        start_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        objects.stop_button,
        stop_button_event,
        LV_EVENT_CLICKED,
        NULL
    );

    // =================================================
    // SET INITIAL MAIN DROPDOWN
    // =================================================

    lv_dropdown_set_selected(
        objects.main_panel_select_option_save_dropdown,
        selectedMainSetting
    );

    // =================================================
    // INITIAL MAIN VALUES
    // =================================================

    loadSelectedSettingToMain();

    // =================================================
    // SHOW MAIN PANEL
    // =================================================


show_splash_screen();

Serial.println("Spin Coater UI ready");

    // =================================================
    // MOTOR TIMING
    // =================================================

    lastRPMTime = millis();
    lastControlTime = millis();

    // =================================================
    // FREERTOS TASKS
    // =================================================

    // LVGL task on Core 1.
    // All LVGL calls remain in this task.
    xTaskCreatePinnedToCore(
        uiTask,
        "UI_Task",
        8192,
        NULL,
        2,
        &uiTaskHandle,
        1
    );

    // Motor control on Core 0.
    // No UART and no long blocking operations.
    xTaskCreatePinnedToCore(
        motorTask,
        "Motor_Control",
        4096,
        NULL,
        2,
        &motorTaskHandle,
        0
    );

    Serial.println("FreeRTOS UI + local motor tasks started");

}

// =====================================================
// LOOP
// =====================================================

void loop()
{
    // All application work is handled by FreeRTOS tasks.
    // Arduino loop is intentionally kept empty.
    vTaskDelay(pdMS_TO_TICKS(10));
}
