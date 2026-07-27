#pragma once

#include "ac_scheduler.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct AcInfrared AcInfrared;

/** Allocate infrared resources and open the Storage record. */
AcInfrared* ac_infrared_alloc(void);

/** Release loaded signals, FlipperFormat helpers, and the Storage record. */
void ac_infrared_free(AcInfrared* infrared);

/** Preload both AC commands from the configured saved remote buttons. */
bool ac_infrared_load(AcInfrared* infrared, char* error, size_t error_size);

/** Transmit the command matching the requested AC state. */
bool ac_infrared_transmit(AcInfrared* infrared, AcState state, char* error, size_t error_size);
