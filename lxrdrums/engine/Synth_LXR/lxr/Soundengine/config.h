/*
 * config.h
 *
 *  Created on: 01.04.2012
 *      Author: Julian Schmidt
 *
 *      Don't forget to make a rebuild all after you change settings in this file
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#ifndef CONFIG_SOUNDENGINE_H_
#define CONFIG_SOUNDENGINE_H_

#include "stm32f7xx_hal.h"
#include <stdio.h>

//#define UNUSED(x) (void)(x)

#define true 1
#define false 0

#define CALC_SAMPLES_IN_INTERRUPT 1
#define USE_INTERRUPT_ONLY_DURING_SAVE_PROJ 1
#define USE_BIG_SAMPLE_BUFFER 1

#define SMOOTH_MIXER_VOL_PAN_DRIVE 1

#define UART_DEBUG_ECHO_MODE 0

//CPU load meter for development builds, set with cmake -DLXR_CPU_METER=ON (see CpuMeter.h)
#ifndef CPU_METER
#define CPU_METER 0
#endif

#if USE_BIG_SAMPLE_BUFFER
#define OUTPUT_DMA_SIZE 64
#else
#define OUTPUT_DMA_SIZE 32
#endif
#define DMA_MASK ((OUTPUT_DMA_SIZE*2)-1)
#define UNIT_GAIN_DRIVE 0
#define SET_PARAM_ARRAY_IN_PARSER 1
#define USE_FILTER_DRIVE 0
#define CALC_TONE_CONTROL 0

//if 1 the amp EGs will be calculated on a per sample basis
//takes too much calcuklation time
//if 0 they are calculated for each dma buffer once, only a smoothing LP will be calculated in the sync loop
#define AMP_EG_SYNC 0

//if 1 the 3 drum voices will have the option to mix the mod osc with the main osc, instead of modulating it
#define ENABLE_MIX_OSC 1
#define ENABLE_DRUM_SVF 1

#define EG_SPEED 	1;//0.04125f
#define PITCH_AMOUNT_FACTOR 32


#define USE_DAC2	1	/**< 1: use 2nd DAC 4344 - 0: use ADC 5343*/

#define USE_SD_CARD 1 //used by the user sample memory

//wether to interpolate the oscillator wavetables or not
#define INTERPOLATE_OSC 1
#define INTERPOLATE_FM_OSC 1

#define USE_BOOTLOADER 1 	// if 1 the image will be loaded to offset 0x4000 (you also have to change stm32_flash.ld manually!!!)


#define SYNTH_FS 			I2S_AUDIOFREQ_44K		  	// the requested I2S samplerate => 44.1 kHz

// MDT: the Teensy Audio rate (the LXR runs at 44140 Hz: its I2S prescaler)
#define REAL_FS				(float)44100.f
#define REAL_FS2			(REAL_FS/2.f)
													  	// this is the real sample rate our system is running
//the number of drum voices (bd tom1 tom2)
#define NUM_DRUM_VOICES 3

#if DMA_MODE_ACTIVE
#define OUTPUT_DMA_SIZE 16
#endif

//typedef  uint8_t bool;

// Helper to declare variable in CCM memory, pre-loaded from flash at startup
// MDT: no TCM sections: the default sections (on the Teensy 4: DTCM and ITCM)
#define INDTCM

// Helper to declare variable in CCM memory that is zeroed at startup
#define INDTCMZ

#define IN_ITCM_RAM

// MDT: __fp16 (the FX buffer) is ARM's; GCC has _Float16 elsewhere (the
// Teensy builds with -mfp16-format=ieee: library.json)
#if !defined(__arm__) && !defined(__aarch64__) && !defined(__clang__)
#define __fp16 _Float16
#endif

// MDT: the FX buffer (64 KB) in RAM2 on the Teensy 4 (DMAMEM, not cleared at
// startup: FxBus_init() clears it)
#if defined(__IMXRT1062__)
#define LXR_FX_RAM __attribute__((section(".dmabuffers"), used))
#else
#define LXR_FX_RAM
#endif

// MDT: the FX code in flash on the Teensy 4 (FLASHMEM), but for the FX bus
// itself and the plain delay: RAM1 has no room for it (ITCM)
#if defined(__IMXRT1062__)
#define LXR_FX_FLASH __attribute__((section(".flashmem")))
#else
#define LXR_FX_FLASH
#endif

// MDT: the large tables (wavetables, transient samples) in flash on the
// Teensy 4, whose const data is in RAM otherwise (PROGMEM)
#if defined(__IMXRT1062__)
#define LXR_FLASH_TABLE __attribute__((section(".progmem")))
#else
#define LXR_FLASH_TABLE
#endif

#endif /* CONFIG_H_ */
