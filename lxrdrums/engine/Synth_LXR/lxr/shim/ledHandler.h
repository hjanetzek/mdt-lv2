/*
 * ledHandler.h - stands in for the LXR front panel's LEDs (none here; the
 * plugin's panel outputs show the sequencer's state). Not part of the LXR
 * firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "lxr_glue.h"

#define LED_START_STOP 0
enum { LED_STEP1 = 1, LED_STEP2, LED_STEP3, LED_STEP4, LED_STEP5, LED_STEP6, LED_STEP7, LED_STEP8,
       LED_STEP9, LED_STEP10, LED_STEP11, LED_STEP12, LED_STEP13, LED_STEP14, LED_STEP15, LED_STEP16 };
static inline void led_setValue (uint8_t on, uint8_t ledNr) { (void) on; (void) ledNr; }
static inline void led_setBlinkLed (uint8_t ledNr, uint8_t onOff) { (void) ledNr; (void) onOff; }
static inline uint8_t led_idx2StepLed (uint8_t idx) { return idx; }
static inline void led_clearSequencerLeds (void) {}
static inline void led_updateSeqLeds (void) {}
static inline void led_clearAllBlinkLeds (void) {}
static inline void led_initPatternSelectLeds (void) {}
static inline void led_setActive_step (uint8_t step) { (void) step; }
static inline void led_clearActive_step (void) {}
static inline void led_blinkHandler (void) {}
static inline void led_resetBlinkTimer (void) {}
