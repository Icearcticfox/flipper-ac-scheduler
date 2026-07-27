#include "ac_infrared.h"

#include "ac_config.h"

#include <furi.h>
#include <flipper_format/flipper_format.h>
#include <infrared/signal/infrared_error_code.h>
#include <infrared/signal/infrared_signal.h>
#include <storage/storage.h>

#include <stdio.h>

#define TAG "AcScheduler"

#define AC_INFRARED_FILE_HEADER "IR signals file"
#define AC_INFRARED_FILE_VERSION 1U

struct AcInfrared {
    Storage* storage;
    FlipperFormat* format;
    FuriString* header;
    InfraredSignal* on_signal;
    InfraredSignal* off_signal;
    bool on_loaded;
    bool off_loaded;
};

static void ac_infrared_set_error(char* error, size_t error_size, const char* message) {
    if(error && (error_size > 0U)) {
        snprintf(error, error_size, "%s", message ? message : "IR error");
    }
}

static bool ac_infrared_open_file(AcInfrared* infrared, const char* path) {
    furi_check(infrared);
    furi_check(path);

    if(!flipper_format_buffered_file_open_existing(infrared->format, path)) {
        return false;
    }

    uint32_t version = 0;
    bool ok = flipper_format_read_header(infrared->format, infrared->header, &version);
    ok = ok && furi_string_equal(infrared->header, AC_INFRARED_FILE_HEADER) &&
         (version == AC_INFRARED_FILE_VERSION);

    if(!ok) {
        flipper_format_buffered_file_close(infrared->format);
    }

    return ok;
}

static bool ac_infrared_read_named_signal(
    AcInfrared* infrared,
    const char* path,
    const char* signal_name,
    InfraredSignal* signal) {
    furi_check(infrared);
    furi_check(path);
    furi_check(signal_name);
    furi_check(signal);

    bool loaded = false;
    if(ac_infrared_open_file(infrared, path)) {
        const InfraredErrorCode error =
            infrared_signal_search_by_name_and_read(signal, infrared->format, signal_name);
        loaded = (error == InfraredErrorCodeNone) && infrared_signal_is_valid(signal);
        flipper_format_buffered_file_close(infrared->format);
    }

    return loaded;
}

static bool ac_infrared_load_button(AcInfrared* infrared, const char* signal_name, InfraredSignal* signal) {
    furi_check(infrared);
    furi_check(signal_name);
    furi_check(signal);

    if(ac_infrared_read_named_signal(infrared, AC_CONFIG_REMOTE_PATH, signal_name, signal)) {
        FURI_LOG_I(TAG, "Loaded button '%s' from '%s'", signal_name, AC_CONFIG_REMOTE_PATH);
        return true;
    }

    FURI_LOG_E(TAG, "Failed to load button '%s' from '%s'", signal_name, AC_CONFIG_REMOTE_PATH);
    return false;
}

AcInfrared* ac_infrared_alloc(void) {
    AcInfrared* infrared = malloc(sizeof(AcInfrared));
    if(!infrared) {
        return NULL;
    }

    infrared->storage = furi_record_open(RECORD_STORAGE);
    infrared->format = flipper_format_buffered_file_alloc(infrared->storage);
    infrared->header = furi_string_alloc();
    infrared->on_signal = infrared_signal_alloc();
    infrared->off_signal = infrared_signal_alloc();
    infrared->on_loaded = false;
    infrared->off_loaded = false;

    return infrared;
}

void ac_infrared_free(AcInfrared* infrared) {
    if(!infrared) {
        return;
    }

    if(infrared->on_signal) {
        infrared_signal_free(infrared->on_signal);
    }
    if(infrared->off_signal) {
        infrared_signal_free(infrared->off_signal);
    }
    if(infrared->header) {
        furi_string_free(infrared->header);
    }
    if(infrared->format) {
        flipper_format_free(infrared->format);
    }
    if(infrared->storage) {
        furi_record_close(RECORD_STORAGE);
    }

    free(infrared);
}

bool ac_infrared_load(AcInfrared* infrared, char* error, size_t error_size) {
    if(!infrared) {
        ac_infrared_set_error(error, error_size, "IR init failed");
        return false;
    }

    infrared->on_loaded =
        ac_infrared_load_button(infrared, AC_CONFIG_ON_BUTTON_NAME, infrared->on_signal);
    infrared->off_loaded =
        ac_infrared_load_button(infrared, AC_CONFIG_OFF_BUTTON_NAME, infrared->off_signal);

    if(!infrared->on_loaded && !infrared->off_loaded) {
        ac_infrared_set_error(error, error_size, "No ON/OFF");
    } else if(!infrared->on_loaded) {
        ac_infrared_set_error(error, error_size, "No ON");
    } else if(!infrared->off_loaded) {
        ac_infrared_set_error(error, error_size, "No OFF");
    } else {
        ac_infrared_set_error(error, error_size, "");
    }

    return infrared->on_loaded && infrared->off_loaded;
}

bool ac_infrared_transmit(AcInfrared* infrared, AcState state, char* error, size_t error_size) {
    if(!infrared) {
        ac_infrared_set_error(error, error_size, "Init fail");
        return false;
    }

    const bool on = state == AcStateOn;
    InfraredSignal* signal = on ? infrared->on_signal : infrared->off_signal;
    const bool loaded = on ? infrared->on_loaded : infrared->off_loaded;
    const char* name = on ? AC_CONFIG_ON_BUTTON_NAME : AC_CONFIG_OFF_BUTTON_NAME;

    if((state != AcStateOn) && (state != AcStateOff)) {
        ac_infrared_set_error(error, error_size, "Bad AC state");
        return false;
    }

    if(!loaded || !signal || !infrared_signal_is_valid(signal)) {
        ac_infrared_set_error(error, error_size, on ? "No ON" : "No OFF");
        FURI_LOG_E(TAG, "Signal '%s' is not loaded", name);
        return false;
    }

    FURI_LOG_I(TAG, "Transmit '%s'", name);
    infrared_signal_transmit(signal);
    ac_infrared_set_error(error, error_size, "");
    return true;
}
