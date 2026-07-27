#pragma once

#include <stdint.h>

typedef enum {
    AcStateUnknown,
    AcStateOff,
    AcStateOn,
} AcState;

int32_t ac_scheduler_app(void* p);
