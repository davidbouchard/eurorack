/**
 *
 * Copyright (c) 2013-2017 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include <stdarg.h>
#include <bitset>

#include "PluginParam.h"
#include "dexed_audio_processor.h"

#include "Dexed.h"
#include "synth.h"
#include "freqlut.h"
#include "sin.h"
#include "exp2.h"
#include "env.h"
#include "pitchenv.h"
#include "aligned_buf.h"
#include "fm_op_kernel.h"

FmCore fmCore;

//==============================================================================
DexedAudioProcessor::DexedAudioProcessor() {
#ifdef DEBUG
    Logger *tmp = Logger::getCurrentLogger();
    if ( tmp == NULL ) {
        Logger::setCurrentLogger(FileLogger::createDateStampedLogger("Dexed", "DebugSession-", "log", "DexedAudioProcessor Created"));
    }
    TRACE("Hi");
#endif

    currentNote = -1;
        
    TRACE("controler %s", controllers.opSwitch);
    
    normalizeDxVelocity = false;
    showKeyboard = true;
    
    memset(&voiceStatus, 0, sizeof(VoiceStatus));
    
    controllers.values_[kControllerPitchRange] = 3;
    controllers.values_[kControllerPitchStep] = 0;
    controllers.masterTune = 0;
}

DexedAudioProcessor::~DexedAudioProcessor() {
    TRACE("Bye");
}

const unsigned char init_voice[] = {
49, 99, 28, 68, 98, 98, 91, 0, 39, 54, 50, 1, 1, 4, 0, 2, 82, 0, 1, 0, 7, 
77, 36, 41, 71, 99, 98, 98, 0, 39, 0, 0, 3, 3, 0, 0, 2, 98, 0, 1, 0, 8, 
77, 36, 41, 71, 99, 98, 98, 0, 39, 0, 0, 3, 3, 0, 0, 2, 99, 0, 1, 0, 7, 
77, 76, 82, 71, 99, 98, 98, 0, 39, 0, 0, 3, 3, 0, 0, 2, 99, 0, 1, 0, 5, 
62, 51, 29, 71, 82, 95, 96, 0, 27, 0, 7, 3, 1, 0, 0, 0, 86, 0, 0, 0, 14, 
72, 76, 99, 71, 99, 88, 96, 0, 39, 0, 14, 3, 3, 0, 0, 0, 98, 0, 0, 0, 14, 
84, 95, 95, 60, 50, 50, 50, 50, 21, 7, 1, 37, 0, 5, 0, 0, 4, 3, 24, 66, 82, 
65, 83, 83, 32, 32, 32, 49, 32, 1, 1, 1, 1, 1, 1};

// const char init_voice[] =  {
// 99, 32, 98, 62, 99, 67, 52, 0, 7, 0, 0, 0, 0, 0, 3, 7, 41, 0, 6, 27, 7, 
// 65, 86, 98, 62, 98, 0, 98, 0, 0, 0, 0, 0, 0, 0, 1, 0, 61, 0, 5, 0, 14, 
// 38, 17, 99, 61, 89, 10, 43, 0, 0, 0, 0, 0, 0, 0, 1, 0, 72, 0, 0, 1, 7, 
// 54, 56, 28, 64, 92, 99, 99, 0, 36, 0, 0, 0, 0, 0, 3, 0, 99, 0, 1, 0, 7, 
// 51, 17, 99, 61, 89, 10, 43, 0, 0, 0, 0, 0, 0, 0, 1, 0, 99, 0, 1, 0, 7, 
// 54, 56, 28, 64, 92, 87, 0, 0, 36, 0, 0, 0, 0, 0, 3, 0, 99, 0, 0, 0, 0, 
// 94, 67, 95, 60, 50, 50, 50, 50, 0, 7, 0, 42, 57, 6, 60, 0, 4, 2, 36, 72, 82, 
// 77, 78, 67, 65, 50, 32, 66, 67, 1, 1, 1, 1, 1, 1};

int32_t audiobuf[N];

//==============================================================================
void DexedAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    //Freqlut::init(sampleRate);
    Lfo::init(sampleRate);
    PitchEnv::init(sampleRate);
    Env::init_sr(sampleRate);
    //fx.init(sampleRate);
    
    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].braids_pitch = 0;
    }

    for(unsigned int i=0;i<sizeof(init_voice);i++) {
        data[i] = init_voice[i];
    }

    currentNote = 0;
    controllers.values_[kControllerPitch] = 0x2000;
    controllers.modwheel_cc = 0;
    controllers.foot_cc = 0;
    controllers.breath_cc = 0;
    controllers.aftertouch_cc = 0;

    extra_buf_size = 0;
    
    sustain = false;
    
    lfo.reset(data + 137);
}

void DexedAudioProcessor::releaseResources() {
    currentNote = -1;

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
    }
}

void DexedAudioProcessor::Render(const uint8_t* sync_buffer, int16_t* channelData, size_t numSamples) {
    unsigned int i;
    
    if ( refreshVoice ) {
        for(i=0;i < MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].live )
                voices[i].dx7_note.update(data, pitch_, voices[i].velocity);
        }
        lfo.reset(data + 137);
        refreshVoice = false;
    }

    if (pitch_ != voices[0].braids_pitch) {
        for(i=0;i < MAX_ACTIVE_NOTES;i++) {
            voices[i].braids_pitch = pitch_;
            if ( voices[i].live )
                voices[i].dx7_note.updatePitch(pitch_);
        }
    }

    if (!gatestate_ && voices[0].keydown) {
        keyup();
    } else if (noteStart_) {
        // int16_t ranges from -32768 to 32767
        // midi ranges from 0 to 127
        // parameter + 32768 >> 
        keydown();
        noteStart_ = false;
    }

    // todo apply params

    // flush first events
    for (i=0; i < numSamples && i < extra_buf_size; i++) {
        channelData[i] = extra_buf[i];
    }
    
    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (unsigned int j = 0; j < extra_buf_size - numSamples; j++) {
            extra_buf[j] = extra_buf[j + numSamples];
        }
        extra_buf_size -= numSamples;
    } else {
        for (; i < numSamples; i += N) {
            
            for (int j = 0; j < N; ++j) {
                audiobuf[j] = 0;
            }
            int32_t lfovalue = lfo.getsample();
            int32_t lfodelay = lfo.getdelay();
            
            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                if (voices[note].live) {
                    voices[note].dx7_note.compute(&audiobuf[0], lfovalue, lfodelay, &controllers);
                }
            }
            
            int jmax = numSamples - i;
            for (int j = 0; j < N; ++j) {
                if (j < jmax) {
                    channelData[i + j] = audiobuf[j] >> 13;
                } else {
                    extra_buf[j - jmax] = audiobuf[j] >> 13;
                }
            }
        }
        extra_buf_size = i - numSamples;
    }
}

// void DexedAudioProcessor::processMidiMessage(const MidiMessage *msg) {
//     const uint8 *buf  = msg->getRawData();
//     uint8_t cmd = buf[0];

//     switch(cmd & 0xf0) {
//         case 0x80 :
//             keyup(buf[1]);
//         return;

//         case 0x90 :
//             keydown(buf[1], buf[2]);
//         return;
            
//         case 0xb0 : {
//             int ctrl = buf[1];
//             int value = buf[2];
            
//             switch(ctrl) {
//                 case 1:
//                     controllers.modwheel_cc = value;
//                     controllers.refresh();
//                     break;
//                 case 2:
//                     controllers.breath_cc = value;
//                     controllers.refresh();
//                     break;
//                 case 4:
//                     controllers.foot_cc = value;
//                     controllers.refresh();
//                     break;
//                 case 64:
//                     sustain = value > 63;
//                     if (!sustain) {
//                         for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
//                             if (voices[note].sustained && !voices[note].keydown) {
//                                 voices[note].dx7_note.keyup();
//                                 voices[note].sustained = false;
//                             }
//                         }
//                     }
//                     break;
//                 case 123:
//                     panic();
//                     break;
//             }
//         }
//         return;
            
//         // aftertouch
//         case 0xd0 :
//             controllers.aftertouch_cc = buf[1];
//             controllers.refresh();
//         return;
            
//     }

//     switch (cmd) {
//         case 0xe0 :
//             controllers.values_[kControllerPitch] = buf[1] | (buf[2] << 7);
//         break;
//     }
// }

void DexedAudioProcessor::keydown() {
    uint8_t velo = 127; //(parameter_[0] + 32768) >> 9;
    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }
    
    int note = currentNote;
    for (int i=0; i<MAX_ACTIVE_NOTES; i++) {
        if (!voices[note].keydown) {
            currentNote = (note + 1) % MAX_ACTIVE_NOTES;
            lfo.keydown();  // TODO: should only do this if # keys down was 0
            voices[note].velocity = velo;
            voices[note].sustained = sustain;
            voices[note].keydown = true;
            voices[note].braids_pitch = pitch_;
            voices[note].dx7_note.init(data, pitch_, velo);

            if ( data[136] )
                voices[note].dx7_note.oscSync();
            break;
        }
        note = (note + 1) % MAX_ACTIVE_NOTES;
    }
 
    voices[note].live = true;
}

void DexedAudioProcessor::keyup() {
    int note;
    for (note=0; note<MAX_ACTIVE_NOTES; ++note) {
        if (voices[note].keydown ) {
            voices[note].keydown = false;
            break;
        }
    }
    
    // note not found ?
    if ( note >= MAX_ACTIVE_NOTES ) {
        TRACE("note-off not found???");
        return;
    }
    
    if ( sustain ) {
        voices[note].sustained = true;
    } else {
        voices[note].dx7_note.keyup();
    }
}

void DexedAudioProcessor::panic() {
    for(int i=0;i<MAX_ACTIVE_NOTES;i++) {
        voices[i].keydown = false;
        voices[i].live = false;
        voices[i].dx7_note.oscSync();
    }
}

int DexedAudioProcessor::getEngineType() {
    return engineType;
}

// ====================================================================
bool DexedAudioProcessor::peekVoiceStatus() {
    if ( currentNote == -1 )
        return false;

    // we are trying to find the last "keydown" note
    int note = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (voices[note].keydown) {
            voices[note].dx7_note.peekVoiceStatus(voiceStatus);
            return true;
        }
        if ( --note < 0 )
            note = MAX_ACTIVE_NOTES-1;
    }

    // not found; try a live note
    note = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (voices[note].live) {
            voices[note].dx7_note.peekVoiceStatus(voiceStatus);
            return true;
        }
        if ( --note < 0 )
            note = MAX_ACTIVE_NOTES-1;
    }

    return true;
}