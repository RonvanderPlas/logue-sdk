/*
    moogbass.cpp

    Custom user oscillator for minilogue xd: a Moog-style monophonic bass
    voice.

    Signal flow, once per sample:

        [ bandlimited saw ] --+
                               +--> [ mix ] --> [ 4-pole ladder filter ] --> out
        [ sub-osc, -1 oct  ] --+                        ^
                                                   SHAPE knob = cutoff

    The SHAPE knob is wired directly to the filter cutoff frequency, so
    turning it "opens"/"closes" the filter exactly like the cutoff knob on a
    Minimoog. Everything else below (resonance, sub level) is a fixed
    placeholder constant for now -- those get their own knobs once the core
    sound is dialed in.
*/

#include "userosc.h"

// ---------------------------------------------------------------------------
// Bandlimited waveform helpers (PolyBLEP)
// ---------------------------------------------------------------------------
//
// A naive digital sawtooth/square jumps instantly between -1 and +1 once per
// cycle. That instant jump contains energy at every harmonic, including ones
// above the Nyquist frequency, which fold back down as audible aliasing --
// especially noticeable on a bass oscillator playing low, harmonically rich
// notes.
//
// PolyBLEP ("polynomial band-limited step") patches a small polynomial onto
// the sample(s) right around each discontinuity so the edge is smoothed just
// enough to remove most of that aliasing, without the cost of a full
// bandlimited wavetable.

// t: phase distance from the discontinuity, dt: phase increment/sample
// (i.e. oscillator frequency / samplerate). Returns the correction to add.
static inline float poly_blep(float t, float dt)
{
  if (t < dt) {
    // Just after a rising discontinuity.
    t /= dt;
    return t + t - t * t - 1.f;
  } else if (t > 1.f - dt) {
    // Just before the next discontinuity.
    t = (t - 1.f) / dt;
    return t * t + t + t + 1.f;
  }
  return 0.f;
}

// Bandlimited rising sawtooth. phase in [0,1).
static inline float blep_saw(float phase, float dt)
{
  float saw = 2.f * phase - 1.f;
  saw -= poly_blep(phase, dt);
  return saw;
}

// Bandlimited square wave: the same correction applied at both of its
// discontinuities (phase 0 and phase 0.5).
static inline float blep_square(float phase, float dt)
{
  float sq = (phase < 0.5f) ? 1.f : -1.f;
  sq += poly_blep(phase, dt);

  float shifted = phase + 0.5f;
  shifted -= static_cast<float>(static_cast<uint32_t>(shifted));
  sq -= poly_blep(shifted, dt);
  return sq;
}

// ---------------------------------------------------------------------------
// 4-pole Moog ladder filter emulation
// ---------------------------------------------------------------------------
//
// The real Moog ladder circuit is 4 cascaded one-pole lowpass stages
// (24 dB/octave total) with a slice of the output fed back to the input --
// that feedback is what creates resonance, and driven far enough, self-
// oscillation. This is the well-known Stilson/Smith digital approximation
// of that circuit: a handful of multiplies per sample, cheap enough for a
// microcontroller, and it captures the ladder's resonant "singing"
// character without simulating actual transistor voltages.
class MoogLadder
{
public:
  void reset()
  {
    in1_ = in2_ = in3_ = in4_ = 0.f;
    out1_ = out2_ = out3_ = out4_ = 0.f;
  }

  // cutoff: normalized cutoff frequency, 0 = DC, 1 = Nyquist.
  // resonance: 0 = none, ~4 = edge of self-oscillation.
  inline float process(float input, float cutoff, float resonance)
  {
    // Empirical tuning constants from the original Stilson/Smith model.
    // They compensate for the ladder's frequency-dependent gain and
    // resonance behavior so the cutoff/resonance controls track predictably
    // across the audible range, instead of the filter getting quieter or
    // more/less resonant as you sweep it.
    const float f = cutoff * 1.16f;
    const float fb = resonance * (1.f - 0.15f * f * f);

    input -= out4_ * fb;                    // resonance: feed the last stage back to the input
    input *= 0.35013f * (f * f) * (f * f);  // compensate for passband gain loss at low cutoff

    out1_ = input + 0.3f * in1_ + (1.f - f) * out1_;
    in1_ = input;

    out2_ = out1_ + 0.3f * in2_ + (1.f - f) * out2_;
    in2_ = out1_;

    out3_ = out2_ + 0.3f * in3_ + (1.f - f) * out3_;
    in3_ = out2_;

    out4_ = out3_ + 0.3f * in4_ + (1.f - f) * out4_;
    in4_ = out3_;

    return out4_;
  }

private:
  float in1_ = 0.f, in2_ = 0.f, in3_ = 0.f, in4_ = 0.f;
  float out1_ = 0.f, out2_ = 0.f, out3_ = 0.f, out4_ = 0.f;
};

// ---------------------------------------------------------------------------
// Oscillator state
// ---------------------------------------------------------------------------

namespace {

  struct State {
    float phase0 = 0.f;    // primary saw oscillator phase, wraps in [0,1)
    float phaseSub = 0.f;  // sub-oscillator phase, runs at half the frequency
    float cutoffNorm = 0.f; // filter cutoff, 0..1 = Nyquist; set from SHAPE
    float ampEnv = 1.f;     // note-on declick ramp, 0 (silent) -> 1 (full level)
    MoogLadder ladder;
  };

  State s_state;

  // --- Fixed placeholders. These become real knob-controlled parameters
  //     (Param 1-6 / SHIFT+SHAPE) in a later pass; see the project README. --
  constexpr float k_subLevel    = 0.35f;   // sub-osc mix amount, 0..1
  constexpr float k_resonance   = 1.4f;    // filter resonance, 0..~4 (self-osc near 4)
  constexpr float k_cutoffMinHz = 60.f;    // SHAPE = 0   -> filter fully closed (dark/thumpy)
  constexpr float k_cutoffMaxHz = 7000.f;  // SHAPE = max -> filter fully open (bright/buzzy)

  // Note-on declick ramp length. Resetting the oscillator phase to 0 on
  // every note-on (see OSC_NOTEON) makes the very first sample jump
  // straight to the waveform's edge instead of continuing smoothly from
  // wherever the last note left off -- an instant amplitude step. Ramping
  // the signal up from silence over a couple of milliseconds hides that
  // step; short enough that it still reads as an instant, punchy attack.
  constexpr float k_declickTimeSec = 0.002f; // 2ms
  constexpr float k_declickInc = 1.f / (k_declickTimeSec * k_samplerate);

} // namespace

void OSC_INIT(uint32_t platform, uint32_t api)
{
  (void)platform;
  (void)api;
  s_state = State();
}

void OSC_CYCLE(const user_osc_param_t * const params, int32_t *yn, const uint32_t frames)
{
  // Note pitch (plus any pitch-bend/mod) can change between calls, so the
  // phase increment is recomputed once per block rather than once per note.
  const float w0 = osc_w0f_for_note((params->pitch) >> 8, params->pitch & 0xFF);
  const float wSub = w0 * 0.5f; // one octave below the main oscillator

  float phase0 = s_state.phase0;
  float phaseSub = s_state.phaseSub;
  float ampEnv = s_state.ampEnv;
  const float cutoffNorm = s_state.cutoffNorm;
  MoogLadder &ladder = s_state.ladder;

  q31_t * __restrict y = (q31_t *)yn;
  const q31_t * const y_end = y + frames;

  for (; y != y_end; ++y) {
    const float saw = blep_saw(phase0, w0);
    const float sub = blep_square(phaseSub, wSub);

    float raw = (1.f - k_subLevel) * saw + k_subLevel * sub;
    raw = clip1m1f(raw); // keep the filter's input safely within +/-1
    raw *= ampEnv;        // fade in from the note-on phase reset (see OSC_NOTEON)
    ampEnv = clipmaxf(ampEnv + k_declickInc, 1.f);

    // The filter's own memory is intentionally NOT cleared on note-on (see
    // OSC_NOTEON) -- it keeps whatever state it was already in and carries
    // on smoothly. Filters are continuous-in-time by nature, so re-exciting
    // fresh silence-to-signal into a filter with resonance always causes a
    // knock as it settles; that transient is exactly what you'd hear if a
    // reset were added back here, and it's most obvious at low cutoff where
    // the filter is slowest to settle.
    const float filtered = ladder.process(raw, cutoffNorm, k_resonance);

    *y = f32_to_q31(clip1m1f(filtered));

    phase0 += w0;
    phase0 -= (uint32_t)phase0;
    phaseSub += wSub;
    phaseSub -= (uint32_t)phaseSub;
  }

  s_state.phase0 = phase0;
  s_state.phaseSub = phaseSub;
  s_state.ampEnv = ampEnv;
}

void OSC_NOTEON(const user_osc_param_t * const params)
{
  (void)params;
  // Restart both oscillator phases for a consistent attack on every note,
  // and re-arm the declick ramp (see k_declickInc) to hide the resulting
  // amplitude jump. The filter's memory is deliberately left alone -- see
  // the comment in OSC_CYCLE.
  s_state.phase0 = 0.f;
  s_state.phaseSub = 0.f;
  s_state.ampEnv = 0.f;
}

void OSC_NOTEOFF(const user_osc_param_t * const params)
{
  (void)params;
}

void OSC_PARAM(uint16_t index, uint16_t value)
{
  switch (index) {
  case k_user_osc_param_shape: {
    // SHAPE knob (10-bit: 0..1023) -> filter cutoff frequency.
    // Mapped exponentially, not linearly, because pitch/frequency
    // perception is logarithmic -- an exponential curve makes the knob's
    // sweep from dark to bright feel even across its whole travel.
    const float shape01 = param_val_to_f32(value); // 0..1
    const float cutoffHz = k_cutoffMinHz * fastpowf(k_cutoffMaxHz / k_cutoffMinHz, shape01);
    s_state.cutoffNorm = cutoffHz * 2.f * k_samplerate_recipf; // Hz -> fraction of Nyquist
    break;
  }
  default:
    break;
  }
}
