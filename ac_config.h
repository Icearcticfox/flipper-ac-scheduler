#pragma once

/*
 * Change only these values for your saved IR remote.
 *
 * Path is the saved remote file on the Flipper SD card.
 * Button names must exactly match the button labels inside that remote.
 */
#define AC_CONFIG_REMOTE_PATH "/ext/infrared/Remote.ir"
#define AC_CONFIG_ON_BUTTON_NAME "Ac_on_cool_23_2"
#define AC_CONFIG_OFF_BUTTON_NAME "Ac_off"

/*
 * Debug manual mode.
 *
 * 0: normal scheduler mode.
 * 1: do not send scheduled commands automatically; use physical buttons:
 *    Up = send ON, Down = send OFF, OK = reload IR remote.
 */
#define AC_CONFIG_DEBUG_MANUAL_MODE 0

/*
 * Default schedule values, in minutes.
 *
 * These are used only when /ext/apps_data/ac_scheduler/settings.bin does not exist
 * yet or when the settings file version changes. After that, edit the schedule
 * from inside the app.
 *
 * Day period is [08:00, 23:00).
 * Night period is [23:00, 08:00) and continues through midnight.
 */
#define AC_DEFAULT_DAY_START_HOUR 8
#define AC_DEFAULT_DAY_START_MINUTE 0
#define AC_DEFAULT_NIGHT_START_HOUR 23
#define AC_DEFAULT_NIGHT_START_MINUTE 0

#define AC_DEFAULT_DAY_CYCLE_MINUTES 15
#define AC_DEFAULT_DAY_ON_MINUTES 5

#define AC_DEFAULT_NIGHT_CYCLE_MINUTES 20
#define AC_DEFAULT_NIGHT_ON_MINUTES 5
