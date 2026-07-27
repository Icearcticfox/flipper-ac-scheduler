#include "ac_schedule.h"

#include <furi.h>

#define AC_MINUTES_PER_DAY (24U * 60U)
#define AC_SECONDS_PER_DAY (24U * 60U * 60U)

static uint32_t ac_schedule_seconds_until_cycle_change(
    uint32_t offset_seconds,
    uint32_t cycle_minutes,
    uint32_t on_minutes) {
    const uint32_t cycle_seconds = cycle_minutes * 60U;
    const uint32_t on_seconds = on_minutes * 60U;
    const uint32_t phase_seconds = offset_seconds % cycle_seconds;

    if(phase_seconds < on_seconds) {
        return on_seconds - phase_seconds;
    }

    return cycle_seconds - phase_seconds;
}

AcScheduleStatus ac_schedule_get_status(const DateTime* datetime, const AcScheduleSettings* settings) {
    furi_check(datetime);
    furi_check(settings);

    const uint32_t minutes_since_midnight =
        ((uint32_t)datetime->hour * 60U) + (uint32_t)datetime->minute;
    const uint32_t seconds_since_midnight =
        (minutes_since_midnight * 60U) + (uint32_t)datetime->second;

    AcScheduleStatus status = {
        .should_be_on = false,
        .is_day_period = false,
        .cycle_minutes = settings->night_cycle_minutes,
        .on_minutes = settings->night_on_minutes,
        .minutes_until_change = 0,
        .next_change_hour = 0,
        .next_change_minute = 0,
    };

    uint32_t offset_seconds = 0;
    if((minutes_since_midnight >= settings->day_start_minute) &&
       (minutes_since_midnight < settings->night_start_minute)) {
        status.is_day_period = true;
        status.cycle_minutes = settings->day_cycle_minutes;
        status.on_minutes = settings->day_on_minutes;
        offset_seconds = ((minutes_since_midnight - settings->day_start_minute) * 60U) +
                         (uint32_t)datetime->second;
    } else if(minutes_since_midnight >= settings->night_start_minute) {
        offset_seconds = ((minutes_since_midnight - settings->night_start_minute) * 60U) +
                         (uint32_t)datetime->second;
    } else {
        offset_seconds =
            ((minutes_since_midnight + (AC_MINUTES_PER_DAY - settings->night_start_minute)) *
             60U) +
            (uint32_t)datetime->second;
    }

    const uint32_t cycle_seconds = (uint32_t)status.cycle_minutes * 60U;
    const uint32_t on_seconds = (uint32_t)status.on_minutes * 60U;
    const uint32_t phase_seconds = offset_seconds % cycle_seconds;
    status.should_be_on = phase_seconds < on_seconds;

    const uint32_t seconds_until_change =
        ac_schedule_seconds_until_cycle_change(offset_seconds, status.cycle_minutes, status.on_minutes);
    const uint32_t next_change_seconds =
        (seconds_since_midnight + seconds_until_change) % AC_SECONDS_PER_DAY;

    status.minutes_until_change = (uint16_t)((seconds_until_change + 59U) / 60U);
    status.next_change_hour = (uint8_t)(next_change_seconds / 3600U);
    status.next_change_minute = (uint8_t)((next_change_seconds % 3600U) / 60U);

    return status;
}
