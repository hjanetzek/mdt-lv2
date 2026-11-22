/*
 * MidiParserCC.c
 *
 *  Created on: 02.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 *
 * MDT: the parameter setter of MidiParser.c (midiParser_ccHandler() and
 * what it uses); the MIDI byte parser, routing, MTC and sequencer parts of
 * MidiParser.c are not ported.
 */
#include <Soundengine/MIDI/CortexParser.h>
#include "Soundengine/DSPAudio/VoiceSlot.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/MidiNoteNumbers.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/config.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/Sequencer/clockSync.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/MIDI/valueShaper.h"
#include "Soundengine/DSPAudio/modulationNode.h"
#include "Soundengine/USB/usb_manager.h"
#include "menu/menu.h"
#include "Soundengine/FxBus.h"
#include "Soundengine/MIDI/MidiMessages.h"

static uint16_t midiParser_activeNrpnNumber = 0;
uint8_t midiParser_originalCcValues[0xff];

// 2^(1/12) factor for 1 semitone
#define SEMITONE_UP 1.0594630943592952645618252949463f
#define SEMITONE_DOWN 0.94387431268169349664191315666753f

#define NUM_LFO 6
uint8_t midiParser_selectedLfoVoice[NUM_LFO] = {0,0,0,0,0,0};

//takes a midi value from 0 to 127 and return +/- 50 cent detune factor
float midiParser_calcDetune(uint8_t value)
{
	//linear interpolation between 1(no change) and semitone up/down)
	float frac = (value/127.f -0.5f);
	float cent = 1;
	if(cent>=0)
	{
		cent += frac*(SEMITONE_UP - 1);
	}
	else
	{
		cent += frac*(SEMITONE_UP - 1);
	}
	return cent;
}
//-----------------------------------------------------------
static void midiParser_nrpnHandler(uint16_t value)
{
	//the sound engine handles NRPNs 0-255 (as CC2 messages, data1 is 8 bit);
	//higher ones (e.g. the mutes 1000-1006) are for the front panel only
	if(midiParser_activeNrpnNumber > 0xff)
		return;
	MidiMsg msg2;
	msg2.status = MIDI_CC2;
	msg2.data1 = midiParser_activeNrpnNumber;
	msg2.data2 = value;
	midiParser_ccHandler(msg2,true, 0, 0.f);
}
//-----------------------------------------------------------
/** handle all incoming CCs and invoke action*/
void midiParser_ccHandler(MidiMsg msg, uint8_t updateOriginalValue, uint8_t isFloatModulator, float v)
{

	struct
	{
		uint8_t itg;
		float flt;
	} value;

	if(isFloatModulator)
	{
		value.itg = v*127;
		value.flt = v;
	}
	else
	{
		value.itg = msg.data2;;
		value.flt = value.itg/127.f;
	}

	if((msg.status &0xF0) == MIDI_CC)
	{

		const uint16_t paramNr = msg.data1-1;
		if(updateOriginalValue && !isFloatModulator) {
			// MDT: msg.data1 (= paramNr+1 as uint8_t): paramNr+1 is int, 65536 for data1 0
			midiParser_originalCcValues[msg.data1] = value.itg;
		}

		//engine specific parameters: handled by the engine the slot runs
		if(slot_setRoleParameter(paramNr, value.itg, value.flt, isFloatModulator))
		{
			if(!isFloatModulator)
			{
				modNode_originalValueChanged(paramNr);
			}
			return;
		}

		switch(msg.data1)
		{

		case CC_MOD_WHEEL:

			break;

		case CC_BANK_CHANGE:
			// bank change (coarse) selects kit (sound)
			/*
			 * already send in parseMidiMessage()
			uart_sendFrontpanelByte(MIDI_CC);
			uart_sendFrontpanelByte(CC_BANK_CHANGE);
			uart_sendFrontpanelByte(value.itg);
			*/
			break;

		case NRPN_DATA_ENTRY_COARSE:
			midiParser_nrpnHandler(value.itg);
			return;
			break;

		case NRPN_FINE:
			midiParser_activeNrpnNumber &= ~(0x7f);	//clear lower 7 bit
			midiParser_activeNrpnNumber |= (value.itg&0x7f);
			break;

		case NRPN_COARSE:
			midiParser_activeNrpnNumber &= 0x7f;	//clear upper 7 bit
			midiParser_activeNrpnNumber |= (value.itg<<7);
			break;

		case VOL_SLOPE1:
			slopeEg2_setSlope(&voiceSlots[0].u.drum.oscVolEg,value.itg);

			break;
		case VOL_SLOPE2:
			slopeEg2_setSlope(&voiceSlots[1].u.drum.oscVolEg,value.itg);
			break;
		case VOL_SLOPE3:
			slopeEg2_setSlope(&voiceSlots[2].u.drum.oscVolEg,value.itg);
			break;
		case VOL_SLOPE6:
			slopeEg2_setSlope(&voiceSlots[5].u.hat.oscVolEg,value.itg);
			break;

		case FILTER_FREQ_DRUM1:
		case FILTER_FREQ_DRUM2:
		case FILTER_FREQ_DRUM3:
		{
			const float f = value.flt;//value.itg/127.f;
			//exponential full range freq
			SVF_directSetFilterValue(&voiceSlots[msg.data1-FILTER_FREQ_DRUM1].u.drum.filter,valueShaperF2F(f,FILTER_SHAPER) );
		}
			break;
		case RESO_DRUM1:
		case RESO_DRUM2:
		case RESO_DRUM3:
			SVF_setReso(&voiceSlots[msg.data1-RESO_DRUM1].u.drum.filter, value.flt);
			break;

		case F_OSC1_COARSE:
		{
			//clear upper nibble
			voiceSlots[0].u.drum.osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceSlots[0].u.drum.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceSlots[0].u.drum.osc);
		}
			break;
		case F_OSC2_COARSE:
		{
			//clear upper nibble
			voiceSlots[1].u.drum.osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceSlots[1].u.drum.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceSlots[1].u.drum.osc);
		}
				break;
		case F_OSC3_COARSE:
		{
			//clear upper nibble
			voiceSlots[2].u.drum.osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceSlots[2].u.drum.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceSlots[2].u.drum.osc);
		}
				break;
		case F_OSC4_COARSE:
		{
			//clear upper nibble
			voiceSlots[3].u.snare.osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceSlots[3].u.snare.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceSlots[3].u.snare.osc);
		}
				break;
		case F_OSC5_COARSE:
		{
			//clear upper nibble
			voiceSlots[4].u.cymbal.osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceSlots[4].u.cymbal.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceSlots[4].u.cymbal.osc);
		}
				break;

		case F_OSC1_FINE:
		{
			//clear lower nibble
			voiceSlots[0].u.drum.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[0].u.drum.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[0].u.drum.osc);
		}
			break;
		case F_OSC2_FINE:
		{
			//clear lower nibble
			voiceSlots[1].u.drum.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[1].u.drum.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[1].u.drum.osc);
		}
				break;
		case F_OSC3_FINE:
		{
			//clear lower nibble
			voiceSlots[2].u.drum.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[2].u.drum.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[2].u.drum.osc);
		}
				break;
		case F_OSC4_FINE:
		{
			//clear lower nibble
			voiceSlots[3].u.snare.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[3].u.snare.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[3].u.snare.osc);
		}
				break;

		case F_OSC5_FINE:
		{
			//clear lower nibble
			voiceSlots[4].u.cymbal.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[4].u.cymbal.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[4].u.cymbal.osc);
		}
				break;

		case F_OSC6_FINE:
		{
			//clear lower nibble
			voiceSlots[5].u.hat.osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceSlots[5].u.hat.osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceSlots[5].u.hat.osc);
		}
				break;

		case OSC_WAVE_DRUM1:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceSlots[0].u.drum.osc.waveform = wave;
			}
			else
			{
				voiceSlots[0].u.drum.osc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_DRUM2:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceSlots[1].u.drum.osc.waveform = wave;
			}
			else
			{
				voiceSlots[1].u.drum.osc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_DRUM3:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceSlots[2].u.drum.osc.waveform = wave;
			}
			else
			{
				voiceSlots[2].u.drum.osc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_SNARE:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceSlots[3].u.snare.osc.waveform = wave;
			}
			else
			{
				voiceSlots[3].u.snare.osc.waveform = value.itg;
			}
			break;


		case OSC1_DIST:
#if USE_FILTER_DRIVE
			voiceSlots[0].u.drum.filter.drive = 0.5f + (value.itg/127.f) *6;
#else
			setDistortionShape(&voiceSlots[0].u.drum.distortion,value.flt);
#endif
			break;
		case OSC2_DIST:
#if USE_FILTER_DRIVE
			voiceSlots[1].u.drum.filter.drive = 0.5f + (value.itg/127.f)*6;
#else
			setDistortionShape(&voiceSlots[1].u.drum.distortion,value.flt);
#endif
			break;
		case OSC3_DIST:
#if USE_FILTER_DRIVE
			voiceSlots[3].u.drum.filter.drive = 0.5f + (value.itg/127.f)*6;
#else
			setDistortionShape(&voiceSlots[2].u.drum.distortion,value.flt);
#endif
			break;

		case VELOA1:
			slopeEg2_setAttack(&voiceSlots[0].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
			break;

		case VELOD1:
		{
			slopeEg2_setDecay(&voiceSlots[0].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
		}
			break;

		case VOL1:
			voiceSlots[0].u.drum.vol = value.flt;
			break;

		case PAN1:
			Drum_setPan(&voiceSlots[0].u.drum,value.itg);
			break;

		case VELOA2:
			slopeEg2_setAttack(&voiceSlots[1].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
			break;

			case VELOD2:
			slopeEg2_setDecay(&voiceSlots[1].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
			break;

			case VOL2:
				voiceSlots[1].u.drum.vol = value.flt;
				break;

			case PAN2:
				Drum_setPan(&voiceSlots[1].u.drum,value.itg);
				break;


			case VELOA3:
				slopeEg2_setAttack(&voiceSlots[2].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
				break;

			case VELOD3:
				slopeEg2_setDecay(&voiceSlots[2].u.drum.oscVolEg,value.itg,AMP_EG_SYNC);
				break;

			case VOL3:
				voiceSlots[2].u.drum.vol = value.flt;
				break;

			case PAN3:
				Drum_setPan(&voiceSlots[2].u.drum,value.itg);
				break;

			//snare
			case VOL4:
				voiceSlots[3].u.snare.vol = value.flt;
				break;

			case PAN4:
				Snare_setPan(&voiceSlots[3].u.snare, value.itg);

				break;

			case VELOA4:
				slopeEg2_setAttack(&voiceSlots[3].u.snare.oscVolEg,value.itg,false);
				break;

			case VELOD4:
				slopeEg2_setDecay(&voiceSlots[3].u.snare.oscVolEg,value.itg,false);
				break;

			case SNARE_FILTER_F:
				{
					#if USE_PEAK
						peak_setFreq(&voiceSlots[3].u.snare.filter, value.itg/127.f*20000.f);
					#else
						const float f = value.flt;
						//exponential full range freq
						SVF_directSetFilterValue(&voiceSlots[3].u.snare.filter,valueShaperF2F(f,FILTER_SHAPER) );
					#endif
				}
					break;

			case SNARE_RESO:
#if USE_PEAK
				peak_setGain(&voiceSlots[3].u.snare.filter, value.itg/127.f);
#else
				SVF_setReso(&voiceSlots[3].u.snare.filter, value.flt);
#endif
				break;

			//snare 2
			case VOL5:
				voiceSlots[4].u.cymbal.vol = value.flt;
				break;

			case PAN5:
				Cymbal_setPan(&voiceSlots[4].u.cymbal, value.itg);
				break;

			case VELOA5:
				slopeEg2_setAttack(&voiceSlots[4].u.cymbal.oscVolEg,value.itg,false);
				break;

			case VELOD5:
				slopeEg2_setDecay(&voiceSlots[4].u.cymbal.oscVolEg,value.itg,false);
				break;

			case CYM_WAVE1:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					voiceSlots[4].u.cymbal.osc.waveform = wave;
				}
				else
				{
					voiceSlots[4].u.cymbal.osc.waveform = value.itg;
				}
				break;

			case CYM_FIL_FREQ:
			{
				const float f = value.flt;
				//exponential full range freq
				SVF_directSetFilterValue(&voiceSlots[4].u.cymbal.filter,valueShaperF2F(f,FILTER_SHAPER) );
			}
				break;

			case CYM_RESO:
				SVF_setReso(&voiceSlots[4].u.cymbal.filter, value.flt);
				break;

			case CYM_SLOPE:
				slopeEg2_setSlope(&voiceSlots[4].u.cymbal.oscVolEg,value.itg);
				break;

			// hat
			case WAVE1_HH:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					voiceSlots[5].u.hat.osc.waveform = wave;
				}
				else
				{
					voiceSlots[5].u.hat.osc.waveform = value.itg;
				}
				break;

			case VELOD6:
				voiceSlots[5].decay[0] = slopeEg2_calcDecay(value.itg);
				break;

			case VELOD6_OPEN:
				voiceSlots[5].decay[1] = slopeEg2_calcDecay(value.itg);
				break;

			case HAT_FILTER_F:
				{
					const float f = value.flt;
					//exponential full range freq
					SVF_directSetFilterValue(&voiceSlots[5].u.hat.filter,valueShaperF2F(f,FILTER_SHAPER) );
				}
				break;

			case HAT_RESO:
				SVF_setReso(&voiceSlots[5].u.hat.filter, value.flt);
				break;

			case VOL6:
				voiceSlots[5].u.hat.vol = value.flt;
				break;

			case PAN6:
				HiHat_setPan(&voiceSlots[5].u.hat, value.itg);
				break;

			case F_OSC6_COARSE:
				{
					//clear upper nibble
					voiceSlots[5].u.hat.osc.midiFreq &= 0x00ff;
					//set upper nibble
					voiceSlots[5].u.hat.osc.midiFreq |= value.itg << 8;
					osc_recalcFreq(&voiceSlots[5].u.hat.osc);
				}
				break;

			case VELOA6:
				slopeEg2_setAttack(&voiceSlots[5].u.hat.oscVolEg,value.itg,false);
				break;


			case EG_SNARE1_SLOPE:
				slopeEg2_setSlope(&voiceSlots[3].u.snare.oscVolEg,value.itg);
				break;

			case SNARE_DISTORTION:
				setDistortionShape(&voiceSlots[3].u.snare.distortion,value.flt);
				break;

			case CYMBAL_DISTORTION:
				setDistortionShape(&voiceSlots[4].u.cymbal.distortion,value.flt);
				break;

			case HAT_DISTORTION:
				setDistortionShape(&voiceSlots[5].u.hat.distortion,value.flt);
				break;

			case FREQ_LFO1:
			case FREQ_LFO2:
			case FREQ_LFO3:
				lfo_setFreq(&voiceSlots[msg.data1-FREQ_LFO1].u.drum.lfo,value.flt);
								break;
			case FREQ_LFO4:
				lfo_setFreq(&voiceSlots[3].u.snare.lfo,value.flt);
								break;
			case FREQ_LFO5:
				lfo_setFreq(&voiceSlots[4].u.cymbal.lfo,value.flt);
								break;
			case FREQ_LFO6:
				lfo_setFreq(&voiceSlots[5].u.hat.lfo,value.flt);
				break;

			case AMOUNT_LFO1:
			case AMOUNT_LFO2:
			case AMOUNT_LFO3:
				voiceSlots[msg.data1-AMOUNT_LFO1].u.drum.lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO4:
				voiceSlots[3].u.snare.lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO5:
				voiceSlots[4].u.cymbal.lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO6:
				voiceSlots[5].u.hat.lfo.modTarget.amount = value.flt;
				break;

			case VOICE_DECIMATION1:
			case VOICE_DECIMATION2:
			case VOICE_DECIMATION3:
			case VOICE_DECIMATION4:
			case VOICE_DECIMATION5:
			case VOICE_DECIMATION6:
			case VOICE_DECIMATION_ALL:
				mixer_decimation_rate[msg.data1-VOICE_DECIMATION1] = valueShaperF2F(value.flt,-0.7f); ;//valueShaperI2F(value.itg,-0.7f);
				break;

			default:
				break;
		}
		if(!isFloatModulator)
		{
			modNode_originalValueChanged(paramNr);
		}
	} //msg.status == MIDI_CC

	else //MIDI_CC2
	{
		const uint16_t paramNr = msg.data1 + 1 + 127;

		//(paramNr goes up to 383: NRPNs 128-255, e.g. the mutes 200-206)
		if(updateOriginalValue && !isFloatModulator && paramNr < sizeof(midiParser_originalCcValues)) {
			midiParser_originalCcValues[paramNr] = value.itg;
		}

		//engine specific parameters: handled by the engine the slot runs
		if(slot_setRoleParameter(paramNr, value.itg, value.flt, isFloatModulator))
		{
			if(!isFloatModulator)
			{
				modNode_originalValueChanged(paramNr);
			}
			return;
		}
		switch(msg.data1)
		{

			case CC2_TRANS1_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[0].u.drum.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[0].u.drum.transGen, value.itg);
				}
				break;

			case CC2_TRANS1_VOL:
				voiceSlots[0].u.drum.transGen.volume = value.flt;
				break;

			case CC2_TRANS1_FREQ:
				voiceSlots[0].u.drum.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;// range about  0.25 to 4 => 1/4 to 1*4
				break;

			case CC2_TRANS2_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[1].u.drum.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[1].u.drum.transGen, value.itg);
				}
				break;

			case CC2_TRANS2_VOL:
				voiceSlots[1].u.drum.transGen.volume = value.flt;
				break;

			case CC2_TRANS2_FREQ:
				voiceSlots[1].u.drum.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS3_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[2].u.drum.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[2].u.drum.transGen, value.itg);
				}
				break;

			case CC2_TRANS3_VOL:
				voiceSlots[2].u.drum.transGen.volume = value.flt;
				break;

			case CC2_TRANS3_FREQ:
				voiceSlots[2].u.drum.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS4_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[3].u.snare.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[3].u.snare.transGen, value.itg);
				}
				break;

			case CC2_TRANS4_VOL:
				voiceSlots[3].u.snare.transGen.volume = value.flt;
				break;
			case CC2_TRANS4_FREQ:
				voiceSlots[3].u.snare.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS5_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[4].u.cymbal.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[4].u.cymbal.transGen, value.itg);
				}
				break;

			case CC2_TRANS5_VOL:
				voiceSlots[4].u.cymbal.transGen.volume = value.flt;
				break;
			case CC2_TRANS5_FREQ:
				voiceSlots[4].u.cymbal.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS6_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceSlots[5].u.hat.transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceSlots[5].u.hat.transGen, value.itg);
				}
				break;

			case CC2_TRANS6_VOL:
				voiceSlots[5].u.hat.transGen.volume = value.flt;
				break;
			case CC2_TRANS6_FREQ:
				voiceSlots[5].u.hat.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_FILTER_TYPE_1:
			case CC2_FILTER_TYPE_2:
			case CC2_FILTER_TYPE_3:
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					voiceSlots[msg.data1-CC2_FILTER_TYPE_1].u.drum.filterType = type;
				}
				else
				{
					voiceSlots[msg.data1-CC2_FILTER_TYPE_1].u.drum.filterType = value.itg+1;
				}
				break;

			case CC2_FILTER_TYPE_4:
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					voiceSlots[3].u.snare.filterType = type;
				}
				else
				{
					voiceSlots[3].u.snare.filterType = value.itg + 1; // +1 because 0 is filter off which results in silence
				}
				break;

			case CC2_FILTER_TYPE_5:
				//cymbal filter
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					voiceSlots[4].u.cymbal.filterType = type;
				}
				else
				{
					voiceSlots[4].u.cymbal.filterType = value.itg+1;
				}
				break;

			case CC2_FILTER_TYPE_6:
				//Hihat filter
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					voiceSlots[5].u.hat.filterType = type;
				}
				else
				{
					voiceSlots[5].u.hat.filterType = value.itg+1;
				}
				//SVF_reset(&voiceSlots[5].u.hat.filter);
				break;

			case CC2_FILTER_DRIVE_1:
			case CC2_FILTER_DRIVE_2:
			case CC2_FILTER_DRIVE_3:
#if UNIT_GAIN_DRIVE
				voiceSlots[msg.data1-CC2_FILTER_DRIVE_1].u.drum.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&voiceSlots[msg.data1-CC2_FILTER_DRIVE_1].u.drum.filter, value.flt);
#endif
				break;
			case CC2_FILTER_DRIVE_4:
#if UNIT_GAIN_DRIVE
				voiceSlots[3].u.snare.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&voiceSlots[3].u.snare.filter, value.flt);
#endif

				break;
			case CC2_FILTER_DRIVE_5:
#if UNIT_GAIN_DRIVE
				voiceSlots[4].u.cymbal.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&voiceSlots[4].u.cymbal.filter, value.flt);
#endif

				break;
			case CC2_FILTER_DRIVE_6:
#if UNIT_GAIN_DRIVE
				voiceSlots[5].u.hat.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&voiceSlots[5].u.hat.filter, value.flt);
#endif

				break;

			case CC2_VOLUME_MOD_ON_OFF1:
			case CC2_VOLUME_MOD_ON_OFF2:
			case CC2_VOLUME_MOD_ON_OFF3:
				voiceSlots[msg.data1-CC2_VOLUME_MOD_ON_OFF1].u.drum.volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF4:
				voiceSlots[3].u.snare.volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF5:
				voiceSlots[4].u.cymbal.volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF6:
				voiceSlots[5].u.hat.volumeMod = value.itg != 0;
				break;

			case CC2_VELO_MOD_AMT_1:
			case CC2_VELO_MOD_AMT_2:
			case CC2_VELO_MOD_AMT_3:
			case CC2_VELO_MOD_AMT_4:
			case CC2_VELO_MOD_AMT_5:
			case CC2_VELO_MOD_AMT_6:
				velocityModulators[msg.data1-CC2_VELO_MOD_AMT_1].amount = value.flt;
				break;

			case CC2_WAVE_LFO1:
			case CC2_WAVE_LFO2:
			case CC2_WAVE_LFO3:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					voiceSlots[msg.data1-CC2_WAVE_LFO1].u.drum.lfo.waveform = wave;
				}
				else
				{
					voiceSlots[msg.data1-CC2_WAVE_LFO1].u.drum.lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO4:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					voiceSlots[3].u.snare.lfo.waveform = wave;
				}
				else
				{
					voiceSlots[3].u.snare.lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO5:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					voiceSlots[4].u.cymbal.lfo.waveform = wave;
				}
				else
				{
					voiceSlots[4].u.cymbal.lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO6:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					voiceSlots[5].u.hat.lfo.waveform = wave;
				}
				else
				{
					voiceSlots[5].u.hat.lfo.waveform = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO1:
			case CC2_RETRIGGER_LFO2:
			case CC2_RETRIGGER_LFO3:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					voiceSlots[msg.data1-CC2_RETRIGGER_LFO1].u.drum.lfo.retrigger = voice;
				}
				else
				{
					voiceSlots[msg.data1-CC2_RETRIGGER_LFO1].u.drum.lfo.retrigger = value.itg;
				}
				break;
			case CC2_RETRIGGER_LFO4:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					voiceSlots[3].u.snare.lfo.retrigger = voice;
				}
				else
				{
					voiceSlots[3].u.snare.lfo.retrigger = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO5:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					voiceSlots[4].u.cymbal.lfo.retrigger = voice;
				}
				else
				{
					voiceSlots[4].u.cymbal.lfo.retrigger = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO6:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					voiceSlots[5].u.hat.lfo.retrigger = voice;
				}
				else
				{
					voiceSlots[5].u.hat.lfo.retrigger = value.itg;
				}
				break;

			case CC2_SYNC_LFO1:
			case CC2_SYNC_LFO2:
			case CC2_SYNC_LFO3:
				lfo_setSync(&voiceSlots[msg.data1-CC2_SYNC_LFO1].u.drum.lfo, value.itg);
				break;
			case CC2_SYNC_LFO4:
				lfo_setSync(&voiceSlots[3].u.snare.lfo, value.itg);
				break;
			case CC2_SYNC_LFO5:
				lfo_setSync(&voiceSlots[4].u.cymbal.lfo, value.itg);
				break;
			case CC2_SYNC_LFO6:
				lfo_setSync(&voiceSlots[5].u.hat.lfo, value.itg);
				break;

			case CC2_VOICE_LFO1:
			case CC2_VOICE_LFO2:
			case CC2_VOICE_LFO3:
			case CC2_VOICE_LFO4:
			case CC2_VOICE_LFO5:
			case CC2_VOICE_LFO6:
				break;

			case CC2_TARGET_LFO1:
			case CC2_TARGET_LFO2:
			case CC2_TARGET_LFO3:
			case CC2_TARGET_LFO4:
			case CC2_TARGET_LFO5:
			case CC2_TARGET_LFO6:
				break;



			case CC2_OFFSET_LFO1:
			case CC2_OFFSET_LFO2:
			case CC2_OFFSET_LFO3:
				voiceSlots[msg.data1-CC2_OFFSET_LFO1].u.drum.lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO4:
				voiceSlots[3].u.snare.lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO5:
				voiceSlots[4].u.cymbal.lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO6:
				voiceSlots[5].u.hat.lfo.phaseOffset = value.flt * 0xffffffff;
				break;

			case CC2_AUDIO_OUT1:
			case CC2_AUDIO_OUT2:
			case CC2_AUDIO_OUT3:
			case CC2_AUDIO_OUT4:
			case CC2_AUDIO_OUT5:
			case CC2_AUDIO_OUT6:
				mixer_audioRouting[msg.data1-CC2_AUDIO_OUT1] = value.itg;
				break;

				/*
			case CC2_MIDI_NOTE1:
			case CC2_MIDI_NOTE2:
			case CC2_MIDI_NOTE3:
			case CC2_MIDI_NOTE4:
			case CC2_MIDI_NOTE5:
			case CC2_MIDI_NOTE6:
			case CC2_MIDI_NOTE7:
				// set the note override for the voice. 0 means use the note value, anything else means
				// that the note will always play with that note
				//midi_NoteOverride[msg.data1-CC2_MIDI_NOTE1] = value.itg;
				break;
				*/

			case CC2_MUTE_1:
			case CC2_MUTE_2:
			case CC2_MUTE_3:
			case CC2_MUTE_4:
			case CC2_MUTE_5:
			case CC2_MUTE_6:
			case CC2_MUTE_7:
			{
				const uint8_t voiceNr = msg.data1 - CC2_MUTE_1;
				if(value.itg == 0)
				{
					seq_setMute(voiceNr,0);
				}
				else
				{
					seq_setMute(voiceNr,1);
				}

			}
				break;

			case CC2_PWM_1:
			case CC2_PWM_2:
			case CC2_PWM_3:
				voiceSlots[msg.data1-CC2_PWM_1].u.drum.pwm = value.flt;
				break;
			case CC2_PWM_4:
				voiceSlots[3].u.snare.pwm = value.flt;
				break;
			case CC2_PWM_5:
				voiceSlots[4].u.cymbal.pwm = value.flt;
				break;
			case CC2_PWM_6:
				voiceSlots[5].u.hat.pwm = value.flt;
				break;

			case CC2_FX_TYPE:
				//out of range (NRPN/modulation 0-127): the FX page would read past its sub pages
#if USE_REVERB
				if(value.itg > FX_REVERB) break;
#else
				if(value.itg > FX_DELAY) break;
#endif
#if 1
#if USE_REVERB
				if((value.itg == FX_REVERB) && (fxBus.fxType != value.itg)) FxBus_prepareFxChange();
#else
				if( fxBus.fxType != value.itg) FxBus_prepareFxChange();
#endif
				fxBus.fxType = value.itg;
				FxBus_updateParameter();
				//change visible parameters to selected FX type
				if(menu_activePage == FX_MODE_PAGE)
				{
					if(menu_getSubPage() != fxBus.fxType)
						menu_switchSubPage(fxBus.fxType, 1 );
				}
#endif
				break;
			case CC2_FX_ROUTING:
				fxBus.audioRouting = value.itg;
				break;
			case CC2_FX_AMOUNT:
				fxBus.dryWet = value.itg/100.f;
				break;

			case CC2_FX_DIST_TYPE:
				fxBus.distortion.distortionType = value.itg;
				break;

			case CC2_FX_P1:
				FxBus_setParameter(0, value.flt);
				break;

			case CC2_FX_P2:
				FxBus_setParameter(1, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_P3:
				FxBus_setParameter(2, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_P4:
				FxBus_setParameter(3, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_RATIO:
				Compressor_setRatio(&fxBus.compressor, value.itg);
				break;

			case CC2_FX_WAVE:
				Ringmod_setWave(&fxBus.ringmod, value.itg);
				break;

			case CC2_FX_REVERB_TYPE:

				break;

			case CC2_FX_DELAY_TYPE:
				fxBus.delay.delayType = value.itg;
			break;
			case PAR_SLOT_ENGINE-128:
				//command, not a sound parameter: value = slot*4 + engine. Only from
				//a received NRPN (midiParser_nrpnHandler): kit loads send this byte
				//too, and old kits can hold any value in it (it once was a MIDI note
				//setting); step automation uses MIDI_CC2 without updateOriginalValue
				if(msg.status == MIDI_CC2 && updateOriginalValue && !isFloatModulator)
				{
					slot_setEngine(value.itg>>2, value.itg&3);
				}
				break;

			default:
				break;
		}
		if(!isFloatModulator)
		{
			modNode_originalValueChanged(paramNr);
		}
	}
}
