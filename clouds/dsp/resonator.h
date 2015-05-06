// Copyright 2014 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Resonator.

#ifndef CLOUDS_DSP_RESONATOR_H_
#define CLOUDS_DSP_RESONATOR_H_

#include "stmlib/stmlib.h"
#include "stmlib/utils/random.h"
#include "stmlib/dsp/units.h"
#include "clouds/dsp/fx/fx_engine.h"
#include "clouds/resources.h"

using namespace stmlib;

namespace clouds {

const float chords[3][18] =
  {
    { 0.0f, 4.0f/128.0f, 16.0f/128.0f, 4.0f/128.0f, 4.0f/128.0f,
      12.0f, 12.0f, 4.0f, 4.0f,
      3.0f, 3.0f, 2.0f, 4.0f,
      3.0f, 4.0f, 3.0f, 4.0f,
      4.0f },
    { 0.0f, 8.0f/128.0f, 32.0f/128.0f, 7.0f, 12.0f,
      24.0f, 7.0f, 7.0f, 7.0f,
      7.0f, 7.0f, 7.0f, 7.0f,
      7.0f, 7.0f, 7.0f, 7.0f,
      7.0f },
    { 0.0f, 12.0f/128.0f, 48.0f/128.0f, 7.0f + 4.0f/128.0f, 12.0f + 4.0f/128.0f,
      36.0f, 19.0f, 12.0f, 11.0f,
      10.0f, 12.0f, 12.0f, 12.0f,
      14.0f, 14.0f, 16.0f, 16.0f,
      16.0f }
  };

#define PLATEAU 2.0f

inline float InterpolatePlateau(const float* table, float index, float size) {
  index *= size;
  MAKE_INTEGRAL_FRACTIONAL(index)
  float a = table[index_integral];
  float b = table[index_integral + 1];
  if (index_fractional < 1.0f/PLATEAU)
    return a + (b - a) * index_fractional * PLATEAU;
  else
    return b;
}

class Resonator {
 public:
  Resonator() { }
  ~Resonator() { }

  void Init(uint16_t* buffer) {
    engine_.Init(buffer);
    feedback_ = 0.0f;
    damp_ = 0.6f;
    base_pitch_ = 0.0f;
    chord_ = 0.0f;
    spread_amount_ = 0.0f;
    stereo_ = 0.0f;
    burst_time_ = 0.0f;
    burst_damp_ = 1.0f;
    burst_comb_ = 1.0f;
    voice_ = false;
    for (int i=0; i<3; i++)
      spread_delay_[i] = Random::GetFloat() * 3999;
  }

#define MAX_COMB 1000
#define BASE_PITCH 220.0f

  void Process(FloatFrame* in_out, size_t size) {

    typedef E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB,
      E::Reserve<200,          /* bc */
      E::Reserve<4000,          /* bd */
      E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB,
      E::Reserve<MAX_COMB > > > > > > > > > > Memory;
    E::DelayLine<Memory, 0> c00;
    E::DelayLine<Memory, 1> c10;
    E::DelayLine<Memory, 2> c20;
    E::DelayLine<Memory, 3> c30;
    E::DelayLine<Memory, 4> bc;
    E::DelayLine<Memory, 5> bd;
    E::DelayLine<Memory, 6> c01;
    E::DelayLine<Memory, 7> c11;
    E::DelayLine<Memory, 8> c21;
    E::DelayLine<Memory, 9> c31;
    E::Context c;

    if (trigger_ && !previous_trigger_) {
      voice_ = !voice_;
    }

    pitch_[voice_] = base_pitch_;

    float c_delay[4][2];

    for (int v=0; v<2; v++) {
      c_delay[0][v] = 32000.0f / BASE_PITCH / SemitonesToRatio(pitch_[v]);
      CONSTRAIN(c_delay[0][v], 0, MAX_COMB);
      for (int p=1; p<4; p++) {
        float pitch = InterpolatePlateau(chords[p-1], chord_, 16);
        c_delay[p][v] = c_delay[0][v] / SemitonesToRatio(pitch);
        CONSTRAIN(c_delay[p][v], 0, MAX_COMB);
      }
    }

    if (trigger_ && !previous_trigger_) {
      previous_trigger_ = trigger_;
      burst_time_ = c_delay[0][voice_];
      burst_time_ *= 2.0f * burst_duration_;

      for (int i=0; i<3; i++)
        spread_delay_[i] = Random::GetFloat() * (bd.length - 1);
    }

    while (size--) {
      engine_.Start(&c);

      burst_time_--;
      float burst_gain = burst_time_ > 0.0f ? 1.0f : 0.0f;

      // burst noise generation
      c.Read((Random::GetFloat() * 2.0f - 1.0f), burst_gain);
      // goes through comb and lp filters
      const float comb_fb = 0.6f - burst_comb_ * 0.4f;
      float comb_del = burst_comb_ * bc.length;
      if (comb_del <= 1.0f) comb_del = 1.0f;
      c.InterpolateHermite(bc, comb_del, comb_fb);
      c.Write(bc, 1.0f);
      c.Lp(burst_lp[0], burst_damp_);
      c.Lp(burst_lp[1], burst_damp_);

      c.Read(in_out->l + in_out->r, 1.0f);
      c.Write(bd, 0.0f);

#define COMB(pre, part, voice, vol)                                     \
      c.Load(0.0f);                                                     \
      c.Read(bd, pre * spread_amount_, vol);                            \
      c.InterpolateHermite(c ## part ## voice,                          \
                           c_delay[part][voice], feedback_);        \
      c.Lp(lp[part][voice], damp_);                                     \
      c.Write(c ## part ## voice, 0.0f);                                \

      /* first voice: */
      COMB(0, 0, 0, !voice_);
      COMB(spread_delay_[0], 1, 0, !voice_);
      COMB(spread_delay_[1], 2, 0, !voice_);
      COMB(spread_delay_[2], 3, 0, !voice_);

      /* second voice: */
      COMB(0, 0, 1, voice_);
      COMB(spread_delay_[0], 1, 1, voice_);
      COMB(spread_delay_[1], 2, 1, voice_);
      COMB(spread_delay_[2], 3, 1, voice_);

      c.Read(c00, 0.20f * (1.0f - stereo_) * (1.0f - separation_));
      c.Read(c10, (0.23f + 0.23f * stereo_) * (1.0f - separation_));
      c.Read(c20, (0.27f * (1.0f - stereo_)) * (1.0f - separation_));
      c.Read(c30, (0.30f + 0.30f * stereo_) * (1.0f - separation_));
      c.Read(c01, 0.20f + 0.20f * stereo_);
      c.Read(c11, 0.23f * (1.0f - stereo_));
      c.Read(c21, 0.27f + 0.27 * stereo_);
      c.Read(c31, 0.30f * (1.0f - stereo_));
      c.Write(in_out->l, 0.0f);

      c.Read(c00, 0.20f + 0.20f * stereo_);
      c.Read(c10, 0.23f * (1.0f - stereo_));
      c.Read(c20, 0.27f + 0.27 * stereo_);
      c.Read(c30, 0.30f * (1.0f - stereo_));
      c.Read(c01, 0.20f * (1.0f - stereo_) * (1.0f - separation_));
      c.Read(c11, (0.23f + 0.23f * stereo_) * (1.0f - separation_));
      c.Read(c21, 0.27f * (1.0f - stereo_) * (1.0f - separation_));
      c.Read(c31, (0.30f + 0.30f * stereo_) * (1.0f - separation_));
      c.Write(in_out->r, 0.0f);

      ++in_out;
    }
  }

  void set_pitch(float pitch) {
    base_pitch_ = pitch;
  }

  void set_trigger(bool trigger) {
    previous_trigger_ = trigger_;
    trigger_ = trigger;
  }

  void set_feedback(float feedback) {
    feedback_ = feedback;
  }

  void set_damp(float damp) {
    damp_ = damp;
  }

  void set_burst_damp(float burst_damp) {
    burst_damp_ = burst_damp;
  }

  void set_burst_comb(float burst_comb) {
    burst_comb_ = burst_comb;
  }

  void set_burst_duration(float burst_duration) {
    burst_duration_ = burst_duration;
  }

  void set_chord(float chord) {
    chord_ = chord;
  }

  void set_spread_amount(float spread_amount) {
    spread_amount_ = spread_amount;
  }

  void set_stereo(float stereo) {
    stereo_ = stereo;
  }

  void set_separation(float separation) {
    separation_ = separation;
  }

 private:
  typedef FxEngine<16384, FORMAT_12_BIT> E;
  E engine_;

  float feedback_;
  float damp_;
  float base_pitch_;
  float chord_;
  float spread_amount_;
  float stereo_;
  float separation_;
  float burst_time_;
  float burst_damp_;
  float burst_comb_;
  float burst_duration_;

  float pitch_[2];

  float spread_delay_[3];

  float burst_lp[2];
  float lp[4][2];

  bool trigger_, previous_trigger_;
  /* voice=0 or 1; for some reason bool doesn't work */
  int32_t voice_;


  DISALLOW_COPY_AND_ASSIGN(Resonator);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_RESONATOR_H_