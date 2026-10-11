#pragma once

#include "driver/i2s_std.h"

/* Does not return during normal operation. */
void hermes_run(i2s_chan_handle_t rx, i2s_chan_handle_t tx);
