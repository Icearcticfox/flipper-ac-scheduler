#pragma once

#include <datetime/datetime.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t day_start_minute;
    uint16_t night_start_minute;
    uint8_t day_cycle_minutes;
    uint8_t day_on_minutes;
    uint8_t night_cycle_minutes;
    uint8_t night_on_minutes;
} AcScheduleSettings;

typedef struct {
    bool should_be_on;
    bool is_day_period;
    uint8_t cycle_minutes;
    uint8_t on_minutes;
    uint16_t minutes_until_change;
    uint8_t next_change_hour;
    uint8_t next_change_minute;
} AcScheduleStatus;

/** Calculate the AC state and next transition from wall-clock time and settings. */
AcScheduleStatus
    ac_schedule_get_status(const DateTime* datetime, const AcScheduleSettings* settings);
