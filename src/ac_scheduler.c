#include "ac_scheduler.h"

#include "ac_config.h"
#include "ac_infrared.h"
#include "ac_schedule.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>
#include <gui/canvas.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>

#include <stdio.h>

#define TAG "AcScheduler"

#define AC_SCHEDULER_QUEUE_SIZE 8U
#define AC_SCHEDULER_TICK_TIMEOUT_MS 1000U
#define AC_SCHEDULER_RETRY_INTERVAL_SECONDS 30U
#define AC_SCHEDULER_SETTINGS_PATH APP_DATA_PATH("settings.bin")
#define AC_SCHEDULER_SETTINGS_MAGIC 0x41435331UL
#define AC_SCHEDULER_SETTINGS_VERSION 1U
#define AC_SCHEDULER_SETTINGS_COUNT 6U
#define AC_SCHEDULER_MAX_CYCLE_MINUTES 120U
#define AC_SCHEDULER_MANUAL_OVERRIDE_SECONDS 10U

typedef enum {
    AcSchedulerEventInput,
    AcSchedulerEventTick,
} AcSchedulerEventType;

typedef struct {
    AcSchedulerEventType type;
    InputEvent input;
} AcSchedulerEvent;

typedef struct {
    uint32_t magic;
    uint16_t version;
    AcScheduleSettings schedule;
} AcSchedulerSettingsFile;

typedef enum {
    AcSchedulerSettingDayStart,
    AcSchedulerSettingNightStart,
    AcSchedulerSettingDayOn,
    AcSchedulerSettingDayCycle,
    AcSchedulerSettingNightOn,
    AcSchedulerSettingNightCycle,
} AcSchedulerSettingIndex;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    AcInfrared* infrared;
    Storage* storage;
    DateTime datetime;
    AcScheduleStatus schedule;
    AcScheduleSettings settings;
    AcScheduleSettings edit_settings;
    AcState commanded_state;
    bool manual_override_active;
    uint32_t manual_override_until_tick;
    bool settings_open;
    uint8_t settings_index;
    uint8_t battery_pct;
    bool error_active;
    uint32_t next_retry_tick;
    char error[64];
} AcSchedulerApp;

static AcState ac_scheduler_expected_state(const AcScheduleStatus* schedule) {
    furi_check(schedule);
    return schedule->should_be_on ? AcStateOn : AcStateOff;
}

static uint32_t ac_scheduler_retry_delta_ticks(void) {
    return AC_SCHEDULER_RETRY_INTERVAL_SECONDS * furi_kernel_get_tick_frequency();
}

static void ac_scheduler_settings_set_defaults(AcScheduleSettings* settings) {
    furi_check(settings);

    settings->day_start_minute =
        ((uint16_t)AC_DEFAULT_DAY_START_HOUR * 60U) + AC_DEFAULT_DAY_START_MINUTE;
    settings->night_start_minute =
        ((uint16_t)AC_DEFAULT_NIGHT_START_HOUR * 60U) + AC_DEFAULT_NIGHT_START_MINUTE;
    settings->day_cycle_minutes = AC_DEFAULT_DAY_CYCLE_MINUTES;
    settings->day_on_minutes = AC_DEFAULT_DAY_ON_MINUTES;
    settings->night_cycle_minutes = AC_DEFAULT_NIGHT_CYCLE_MINUTES;
    settings->night_on_minutes = AC_DEFAULT_NIGHT_ON_MINUTES;
}

static void ac_scheduler_settings_validate(AcScheduleSettings* settings) {
    furi_check(settings);

    if(settings->day_start_minute >= 24U * 60U) settings->day_start_minute = 8U * 60U;
    if(settings->night_start_minute >= 24U * 60U) settings->night_start_minute = 23U * 60U;
    if(settings->night_start_minute <= settings->day_start_minute) {
        settings->day_start_minute = 8U * 60U;
        settings->night_start_minute = 23U * 60U;
    }

    if(settings->day_cycle_minutes == 0U) settings->day_cycle_minutes = 1U;
    if(settings->night_cycle_minutes == 0U) settings->night_cycle_minutes = 1U;
    if(settings->day_cycle_minutes > AC_SCHEDULER_MAX_CYCLE_MINUTES) {
        settings->day_cycle_minutes = AC_SCHEDULER_MAX_CYCLE_MINUTES;
    }
    if(settings->night_cycle_minutes > AC_SCHEDULER_MAX_CYCLE_MINUTES) {
        settings->night_cycle_minutes = AC_SCHEDULER_MAX_CYCLE_MINUTES;
    }

    if(settings->day_on_minutes > settings->day_cycle_minutes) {
        settings->day_on_minutes = settings->day_cycle_minutes;
    }
    if(settings->night_on_minutes > settings->night_cycle_minutes) {
        settings->night_on_minutes = settings->night_cycle_minutes;
    }
}

static bool ac_scheduler_settings_load(AcSchedulerApp* app) {
    furi_check(app);

    File* file = storage_file_alloc(app->storage);
    AcSchedulerSettingsFile stored = {0};
    bool loaded = false;

    do {
        if(!storage_file_open(file, AC_SCHEDULER_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            break;
        }

        const size_t read = storage_file_read(file, &stored, sizeof(stored));
        if(read != sizeof(stored)) {
            break;
        }

        if((stored.magic != AC_SCHEDULER_SETTINGS_MAGIC) ||
           (stored.version != AC_SCHEDULER_SETTINGS_VERSION)) {
            break;
        }

        app->settings = stored.schedule;
        ac_scheduler_settings_validate(&app->settings);
        loaded = true;
    } while(false);

    storage_file_free(file);
    return loaded;
}

static bool ac_scheduler_settings_save(AcSchedulerApp* app) {
    furi_check(app);

    AcSchedulerSettingsFile stored = {
        .magic = AC_SCHEDULER_SETTINGS_MAGIC,
        .version = AC_SCHEDULER_SETTINGS_VERSION,
        .schedule = app->settings,
    };

    File* file = storage_file_alloc(app->storage);
    bool saved = false;

    do {
        if(!storage_file_open(file, AC_SCHEDULER_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            break;
        }

        saved = storage_file_write(file, &stored, sizeof(stored)) == sizeof(stored);
    } while(false);

    storage_file_free(file);

    if(!saved) {
        FURI_LOG_E(TAG, "Failed to save settings");
    }

    return saved;
}

static const char* ac_scheduler_state_text(const AcSchedulerApp* app) {
    furi_check(app);

    if(app->error_active) {
        return "ERROR";
    }

    switch(app->commanded_state) {
    case AcStateOn:
        return "ON";
    case AcStateOff:
        return "OFF";
    case AcStateUnknown:
    default:
        return "UNKNOWN";
    }
}

static bool ac_scheduler_is_debug_manual_mode(void) {
    return AC_CONFIG_DEBUG_MANUAL_MODE != 0;
}

static void ac_scheduler_format_time(char* text, size_t text_size, uint16_t minute_of_day) {
    furi_check(text);
    snprintf(text, text_size, "%02u:%02u", minute_of_day / 60U, minute_of_day % 60U);
}

static void ac_scheduler_draw_settings_line(
    Canvas* canvas,
    uint8_t index,
    uint8_t selected,
    uint8_t y,
    const char* label,
    const char* value) {
    furi_check(canvas);
    furi_check(label);
    furi_check(value);

    char line[32];
    snprintf(line, sizeof(line), "%c%s %s", index == selected ? '>' : ' ', label, value);
    canvas_draw_str(canvas, 0, y, line);
}

static void ac_scheduler_draw_battery(Canvas* canvas, uint8_t pct) {
    furi_check(canvas);

    const uint8_t x = 78;
    const uint8_t y = 1;
    uint8_t fill = pct / 20U;
    if(fill > 5U) fill = 5U;

    canvas_draw_frame(canvas, x, y, 14, 7);
    canvas_draw_box(canvas, x + 14, y + 2, 2, 3);

    if(fill > 0U) {
        canvas_draw_box(canvas, x + 2, y + 2, fill * 2U, 3);
    }
}

static void ac_scheduler_draw_settings(Canvas* canvas, AcSchedulerApp* app) {
    furi_check(canvas);
    furi_check(app);

    char value[10];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 8, "AC Settings");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 70, 8, "^v row <> val");

    ac_scheduler_format_time(value, sizeof(value), app->edit_settings.day_start_minute);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingDayStart, app->settings_index, 18, "Day", value);

    ac_scheduler_format_time(value, sizeof(value), app->edit_settings.night_start_minute);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingNightStart, app->settings_index, 27, "Night", value);

    snprintf(value, sizeof(value), "%um", app->edit_settings.day_on_minutes);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingDayOn, app->settings_index, 36, "D on", value);

    snprintf(
        value,
        sizeof(value),
        "%um",
        app->edit_settings.day_cycle_minutes - app->edit_settings.day_on_minutes);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingDayCycle, app->settings_index, 45, "D off", value);

    snprintf(value, sizeof(value), "%um", app->edit_settings.night_on_minutes);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingNightOn, app->settings_index, 54, "N on", value);

    snprintf(
        value,
        sizeof(value),
        "%um",
        app->edit_settings.night_cycle_minutes - app->edit_settings.night_on_minutes);
    ac_scheduler_draw_settings_line(
        canvas, AcSchedulerSettingNightCycle, app->settings_index, 63, "N off", value);
}

static void ac_scheduler_draw_callback(Canvas* canvas, void* context) {
    AcSchedulerApp* app = context;
    furi_check(canvas);
    furi_check(app);

    char line[40];

    if(app->settings_open) {
        ac_scheduler_draw_settings(canvas, app);
        return;
    }

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 8, "AC Scheduler");
    canvas_set_font(canvas, FontSecondary);
    ac_scheduler_draw_battery(canvas, app->battery_pct);
    snprintf(line, sizeof(line), "%u%%", app->battery_pct);
    canvas_draw_str(canvas, 98, 8, line);

    canvas_set_font(canvas, FontSecondary);

    snprintf(
        line,
        sizeof(line),
        "Time: %02u:%02u:%02u",
        app->datetime.hour,
        app->datetime.minute,
        app->datetime.second);
    canvas_draw_str(canvas, 0, 18, line);

    if(ac_scheduler_is_debug_manual_mode()) {
        snprintf(line, sizeof(line), "Mode: DEBUG MANUAL");
    } else if(app->manual_override_active) {
        snprintf(line, sizeof(line), "Mode: MANUAL");
    } else {
        snprintf(line, sizeof(line), "Period: %s", app->schedule.is_day_period ? "DAY" : "NIGHT");
    }
    canvas_draw_str(canvas, 0, 27, line);

    if(app->error_active && app->error[0] != '\0') {
        snprintf(line, sizeof(line), "State: ERROR %.20s", app->error);
    } else {
        snprintf(line, sizeof(line), "State: %s", ac_scheduler_state_text(app));
    }
    canvas_draw_str(canvas, 0, 36, line);

    if(ac_scheduler_is_debug_manual_mode()) {
        snprintf(line, sizeof(line), "Up: ON  Down: OFF");
    } else {
        snprintf(
            line,
            sizeof(line),
            "ON %um OFF %um",
            app->schedule.on_minutes,
            app->schedule.cycle_minutes - app->schedule.on_minutes);
    }
    canvas_draw_str(canvas, 0, 45, line);

    if(ac_scheduler_is_debug_manual_mode()) {
        snprintf(line, sizeof(line), "OK: reload remote");
    } else {
        snprintf(
            line,
            sizeof(line),
            "Next change: %02u:%02u",
            app->schedule.next_change_hour,
            app->schedule.next_change_minute);
    }
    canvas_draw_str(canvas, 0, 54, line);

    canvas_draw_str(canvas, 0, 63, "OK:menu Hold:send");
}

static void ac_scheduler_input_callback(InputEvent* input, void* context) {
    AcSchedulerApp* app = context;
    furi_check(input);
    furi_check(app);

    AcSchedulerEvent event = {
        .type = AcSchedulerEventInput,
        .input = *input,
    };
    furi_message_queue_put(app->queue, &event, 0);
}

static void ac_scheduler_refresh_time(AcSchedulerApp* app) {
    furi_check(app);
    furi_hal_rtc_get_datetime(&app->datetime);
    app->battery_pct = furi_hal_power_get_pct();
    app->schedule = ac_schedule_get_status(&app->datetime, &app->settings);
}

static bool ac_scheduler_send_state(AcSchedulerApp* app, AcState state, bool manual) {
    furi_check(app);

    if(!manual && (app->commanded_state == state) && !app->error_active) {
        return true;
    }

    char error[sizeof(app->error)];
    error[0] = '\0';

    if(!ac_infrared_transmit(app->infrared, state, error, sizeof(error))) {
        app->error_active = true;
        snprintf(app->error, sizeof(app->error), "%s", error[0] ? error : "Transmit failed");
        app->next_retry_tick = furi_get_tick() + ac_scheduler_retry_delta_ticks();
        FURI_LOG_E(TAG, "Transmit failed: %s", app->error);
        return false;
    }

    app->commanded_state = state;
    app->error_active = false;
    app->error[0] = '\0';
    app->next_retry_tick = 0;
    FURI_LOG_I(TAG, "Commanded state: %s", state == AcStateOn ? "ON" : "OFF");
    return true;
}

static bool ac_scheduler_send_expected(AcSchedulerApp* app, bool manual) {
    furi_check(app);
    return ac_scheduler_send_state(app, ac_scheduler_expected_state(&app->schedule), manual);
}

static bool ac_scheduler_toggle_commanded_state(AcSchedulerApp* app) {
    furi_check(app);

    const AcState next_state = app->commanded_state == AcStateOn ? AcStateOff : AcStateOn;
    if(ac_scheduler_send_state(app, next_state, true)) {
        app->manual_override_active = true;
        app->manual_override_until_tick = furi_get_tick() +
                                          (AC_SCHEDULER_MANUAL_OVERRIDE_SECONDS *
                                           furi_kernel_get_tick_frequency());
        FURI_LOG_I(TAG, "Manual override active for %u seconds", AC_SCHEDULER_MANUAL_OVERRIDE_SECONDS);
        return true;
    }

    return false;
}

static bool ac_scheduler_reload_infrared(AcSchedulerApp* app) {
    furi_check(app);

    char error[sizeof(app->error)];
    error[0] = '\0';

    if(!ac_infrared_load(app->infrared, error, sizeof(error))) {
        app->error_active = true;
        snprintf(app->error, sizeof(app->error), "%s", error[0] ? error : "Load fail");
        app->next_retry_tick = furi_get_tick() + ac_scheduler_retry_delta_ticks();
        FURI_LOG_E(TAG, "IR load failed: %s", app->error);
        return false;
    }

    app->error_active = false;
    app->error[0] = '\0';
    app->next_retry_tick = 0;
    FURI_LOG_I(TAG, "IR remote loaded");
    return true;
}

static uint8_t ac_scheduler_adjust_u8(uint8_t value, int8_t delta, uint8_t min, uint8_t max) {
    int16_t next = (int16_t)value + delta;
    if(next < min) next = min;
    if(next > max) next = max;
    return (uint8_t)next;
}

static uint16_t ac_scheduler_adjust_hour(
    uint16_t minute_of_day,
    int8_t delta,
    uint8_t min_hour,
    uint8_t max_hour) {
    uint8_t hour = (uint8_t)(minute_of_day / 60U);
    hour = ac_scheduler_adjust_u8(hour, delta, min_hour, max_hour);
    return (uint16_t)hour * 60U;
}

static void ac_scheduler_settings_adjust(AcSchedulerApp* app, int8_t delta) {
    furi_check(app);

    AcScheduleSettings* settings = &app->edit_settings;

    switch(app->settings_index) {
    case AcSchedulerSettingDayStart: {
        const uint8_t max_hour = (uint8_t)((settings->night_start_minute / 60U) - 1U);
        settings->day_start_minute =
            ac_scheduler_adjust_hour(settings->day_start_minute, delta, 0U, max_hour);
        break;
    }
    case AcSchedulerSettingNightStart: {
        const uint8_t min_hour = (uint8_t)((settings->day_start_minute / 60U) + 1U);
        settings->night_start_minute =
            ac_scheduler_adjust_hour(settings->night_start_minute, delta, min_hour, 23U);
        break;
    }
    case AcSchedulerSettingDayCycle:
        settings->day_cycle_minutes =
            settings->day_on_minutes +
            ac_scheduler_adjust_u8(
                settings->day_cycle_minutes - settings->day_on_minutes,
                delta,
                settings->day_on_minutes == 0U ? 1U : 0U,
                AC_SCHEDULER_MAX_CYCLE_MINUTES - settings->day_on_minutes);
        break;
    case AcSchedulerSettingDayOn: {
        const uint8_t off_minutes = settings->day_cycle_minutes - settings->day_on_minutes;
        settings->day_on_minutes = ac_scheduler_adjust_u8(
            settings->day_on_minutes,
            delta,
            off_minutes == 0U ? 1U : 0U,
            AC_SCHEDULER_MAX_CYCLE_MINUTES - off_minutes);
        settings->day_cycle_minutes = settings->day_on_minutes + off_minutes;
        break;
    }
    case AcSchedulerSettingNightCycle:
        settings->night_cycle_minutes =
            settings->night_on_minutes +
            ac_scheduler_adjust_u8(
                settings->night_cycle_minutes - settings->night_on_minutes,
                delta,
                settings->night_on_minutes == 0U ? 1U : 0U,
                AC_SCHEDULER_MAX_CYCLE_MINUTES - settings->night_on_minutes);
        break;
    case AcSchedulerSettingNightOn: {
        const uint8_t off_minutes = settings->night_cycle_minutes - settings->night_on_minutes;
        settings->night_on_minutes = ac_scheduler_adjust_u8(
            settings->night_on_minutes,
            delta,
            off_minutes == 0U ? 1U : 0U,
            AC_SCHEDULER_MAX_CYCLE_MINUTES - off_minutes);
        settings->night_cycle_minutes = settings->night_on_minutes + off_minutes;
        break;
    }
    default:
        break;
    }
}

static void ac_scheduler_settings_open(AcSchedulerApp* app) {
    furi_check(app);

    app->edit_settings = app->settings;
    app->settings_index = 0;
    app->settings_open = true;
    FURI_LOG_I(TAG, "Settings opened");
}

static void ac_scheduler_settings_cancel(AcSchedulerApp* app) {
    furi_check(app);

    app->edit_settings = app->settings;
    app->settings_open = false;
    FURI_LOG_I(TAG, "Settings canceled");
}

static void ac_scheduler_settings_apply(AcSchedulerApp* app) {
    furi_check(app);

    app->settings = app->edit_settings;
    ac_scheduler_settings_validate(&app->settings);
    ac_scheduler_settings_save(app);
    app->manual_override_active = false;
    app->settings_open = false;
    ac_scheduler_refresh_time(app);

    if(!ac_scheduler_is_debug_manual_mode()) {
        ac_scheduler_send_expected(app, false);
    }

    FURI_LOG_I(TAG, "Settings applied");
}

static void ac_scheduler_handle_settings_input(AcSchedulerApp* app, const InputEvent* input) {
    furi_check(app);
    furi_check(input);

    if(input->type == InputTypeShort) {
        if(input->key == InputKeyBack) {
            ac_scheduler_settings_cancel(app);
        } else if(input->key == InputKeyOk) {
            ac_scheduler_settings_apply(app);
        } else if(input->key == InputKeyLeft) {
            ac_scheduler_settings_adjust(app, -1);
        } else if(input->key == InputKeyRight) {
            ac_scheduler_settings_adjust(app, 1);
        } else if(input->key == InputKeyUp) {
            app->settings_index =
                (app->settings_index == 0U) ? AC_SCHEDULER_SETTINGS_COUNT - 1U :
                                              app->settings_index - 1U;
        } else if(input->key == InputKeyDown) {
            app->settings_index = (app->settings_index + 1U) % AC_SCHEDULER_SETTINGS_COUNT;
        }
    } else if(input->type == InputTypeRepeat) {
        if(input->key == InputKeyRight) {
            ac_scheduler_settings_adjust(app, 1);
        } else if(input->key == InputKeyLeft) {
            ac_scheduler_settings_adjust(app, -1);
        }
    }

    view_port_update(app->view_port);
}

static void ac_scheduler_retry_if_due(AcSchedulerApp* app) {
    furi_check(app);

    if(!app->error_active) {
        return;
    }

    const uint32_t now = furi_get_tick();
    if((int32_t)(now - app->next_retry_tick) < 0) {
        return;
    }

    if(!ac_scheduler_reload_infrared(app)) {
        app->next_retry_tick = now + ac_scheduler_retry_delta_ticks();
        return;
    }

    if(!ac_scheduler_is_debug_manual_mode()) {
        ac_scheduler_send_expected(app, true);
    }
}

static AcSchedulerApp* ac_scheduler_app_alloc(void) {
    AcSchedulerApp* app = malloc(sizeof(AcSchedulerApp));
    if(!app) {
        return NULL;
    }

    app->gui = NULL;
    app->view_port = NULL;
    app->queue = NULL;
    app->infrared = NULL;
    app->storage = NULL;
    app->commanded_state = AcStateUnknown;
    app->manual_override_active = false;
    app->manual_override_until_tick = 0;
    app->settings_open = false;
    app->settings_index = 0;
    app->battery_pct = 0;
    app->error_active = false;
    app->next_retry_tick = 0;
    app->error[0] = '\0';
    ac_scheduler_settings_set_defaults(&app->settings);
    app->edit_settings = app->settings;

    app->queue = furi_message_queue_alloc(AC_SCHEDULER_QUEUE_SIZE, sizeof(AcSchedulerEvent));
    app->view_port = view_port_alloc();
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->infrared = ac_infrared_alloc();

    furi_check(app->queue);
    furi_check(app->view_port);
    furi_check(app->gui);
    furi_check(app->storage);
    furi_check(app->infrared);

    view_port_draw_callback_set(app->view_port, ac_scheduler_draw_callback, app);
    view_port_input_callback_set(app->view_port, ac_scheduler_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    if(!ac_scheduler_settings_load(app)) {
        ac_scheduler_settings_save(app);
    }
    app->edit_settings = app->settings;
    ac_scheduler_refresh_time(app);

    if(ac_scheduler_reload_infrared(app) && !ac_scheduler_is_debug_manual_mode()) {
        ac_scheduler_send_expected(app, true);
    }

    view_port_update(app->view_port);

    return app;
}

static void ac_scheduler_app_free(AcSchedulerApp* app) {
    if(!app) {
        return;
    }

    if(app->gui && app->view_port) {
        gui_remove_view_port(app->gui, app->view_port);
    }
    if(app->view_port) {
        view_port_free(app->view_port);
    }
    if(app->gui) {
        furi_record_close(RECORD_GUI);
    }
    if(app->infrared) {
        ac_infrared_free(app->infrared);
    }
    if(app->storage) {
        furi_record_close(RECORD_STORAGE);
    }
    if(app->queue) {
        furi_message_queue_free(app->queue);
    }

    free(app);
}

static void ac_scheduler_handle_tick(AcSchedulerApp* app) {
    furi_check(app);

    if(app->settings_open) {
        ac_scheduler_refresh_time(app);
        view_port_update(app->view_port);
        return;
    }

    const AcState old_expected_state = ac_scheduler_expected_state(&app->schedule);
    ac_scheduler_refresh_time(app);
    const AcState expected_state = ac_scheduler_expected_state(&app->schedule);

    if(expected_state != old_expected_state) {
        FURI_LOG_I(TAG, "Expected state transition: %s", expected_state == AcStateOn ? "ON" : "OFF");
    }

    if(app->manual_override_active) {
        const uint32_t now = furi_get_tick();
        if((int32_t)(now - app->manual_override_until_tick) < 0) {
            ac_scheduler_retry_if_due(app);
            view_port_update(app->view_port);
            return;
        }

        app->manual_override_active = false;
        FURI_LOG_I(TAG, "Manual override ended");
    }

    if(!ac_scheduler_is_debug_manual_mode() && (app->commanded_state != expected_state) &&
       !app->error_active) {
        ac_scheduler_send_expected(app, false);
    }

    ac_scheduler_retry_if_due(app);
    view_port_update(app->view_port);
}

int32_t ac_scheduler_app(void* p) {
    UNUSED(p);

    AcSchedulerApp* app = ac_scheduler_app_alloc();
    furi_check(app);

    bool running = true;
    while(running) {
        AcSchedulerEvent event = {
            .type = AcSchedulerEventTick,
        };

        const FuriStatus status =
            furi_message_queue_get(app->queue, &event, AC_SCHEDULER_TICK_TIMEOUT_MS);
        if(status == FuriStatusErrorTimeout) {
            event.type = AcSchedulerEventTick;
        } else if(status != FuriStatusOk) {
            FURI_LOG_E(TAG, "Queue error: %ld", (long)status);
            continue;
        }

        if(event.type == AcSchedulerEventInput) {
            if(app->settings_open) {
                ac_scheduler_handle_settings_input(app, &event.input);
            } else if((event.input.type == InputTypeShort) && (event.input.key == InputKeyBack)) {
                running = false;
            } else if((event.input.type == InputTypeLong) && (event.input.key == InputKeyOk)) {
                ac_scheduler_refresh_time(app);
                if(ac_scheduler_is_debug_manual_mode()) {
                    ac_scheduler_reload_infrared(app);
                } else {
                    if(app->error_active) {
                        ac_scheduler_reload_infrared(app);
                    }
                    ac_scheduler_toggle_commanded_state(app);
                }
                view_port_update(app->view_port);
            } else if(
                ac_scheduler_is_debug_manual_mode() && (event.input.type == InputTypeShort) &&
                (event.input.key == InputKeyUp)) {
                ac_scheduler_send_state(app, AcStateOn, true);
                view_port_update(app->view_port);
            } else if(
                ac_scheduler_is_debug_manual_mode() && (event.input.type == InputTypeShort) &&
                (event.input.key == InputKeyDown)) {
                ac_scheduler_send_state(app, AcStateOff, true);
                view_port_update(app->view_port);
            } else if((event.input.type == InputTypeShort) && (event.input.key == InputKeyOk)) {
                if(ac_scheduler_is_debug_manual_mode()) {
                    ac_scheduler_reload_infrared(app);
                } else {
                    ac_scheduler_settings_open(app);
                }
                view_port_update(app->view_port);
            }
        } else {
            ac_scheduler_handle_tick(app);
        }
    }

    FURI_LOG_I(TAG, "Exit without changing AC state");
    ac_scheduler_app_free(app);
    return 0;
}
