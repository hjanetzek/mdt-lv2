/*
 * buttonHandler.h - stands in for the LXR front panel's buttons: the
 * select mode the sequencer asks for (never the pattern or song select
 * mode here). Not part of the LXR firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "lxr_glue.h"

#define SELECT_MODE_PATTERN 0x08
#define SELECT_MODE_SONG 0x09
static inline uint8_t buttonHandler_getMode (void) { return 0; }
