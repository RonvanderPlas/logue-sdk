/*
    moogbass.cpp

    Custom user oscillator for minilogue xd: a Moog-style monophonic bass
    voice.

    Signal flow, once per sample:

        [ Osc1: saw<->square, detuned ] --+
                                           +--> [ blend ] --+
        [ Osc2: saw<->square, detuned ] --+                +--> [ mix ] --> [ 4-pole ladder filter ] --> out
                                                             |        ^           ^
        [ sub-osc, -1 oct, fixed to Osc1's root pitch ] -----+  SHAPE = cutoff     |
                                                                        SHIFT+SHAPE = resonance

    SHAPE and SHIFT+SHAPE are wired directly to the filter's cutoff and
    resonance, so together they behave like the two main knobs of a
    Minimoog's filter section.

    The oscillator section is two independent saw<->square oscillators
    (Param1/2 = Osc1 Shape/Detune, Param3/4 = Osc2 Shape/Detune), each with
    its own detune in cents, crossfaded together by Param5 ("Blend": 0% =
    only Osc1, 100% = only Osc2, 50% = equal parts of both -- this is how
    two slightly-detuned oscillators "fatten" a sound, since neither ever
    lands in exactly the same place in its cycle as the other, so their
    peaks and zero-crossings constantly drift in and out of alignment,
    which the ear hears as movement/width rather than a single static
    pitch). Param6 ("Sub Mix") blends in the sub-oscillator, which always
    tracks the note's true pitch (unaffected by either oscillator's
    detune) one octave down.
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
    float phase1 = 0.f;    // Osc1 phase, wraps in [0,1)
    float phase2 = 0.f;    // Osc2 phase, wraps in [0,1)
    float phaseSub = 0.f;  // sub-oscillator phase, runs at half the root frequency
    float cutoffNorm = 0.f; // filter cutoff, 0..1 = Nyquist; set from SHAPE
    float resonance = 0.f;  // filter resonance, 0..k_resonanceMax; set from SHIFT+SHAPE
    float osc1Shape = 0.f;  // Osc1 saw->square blend, 0..1; set from Param1
    float osc1DetuneCents = 0.f; // Osc1 pitch offset in cents; set from Param2
    float osc2Shape = 0.f;  // Osc2 saw->square blend, 0..1; set from Param3
    float osc2DetuneCents = 0.f; // Osc2 pitch offset in cents; set from Param4
    float blend = 0.f;      // Osc1<->Osc2 crossfade, 0=Osc1 .. 1=Osc2; set from Param5
    float subLevel = 0.35f; // sub-osc mix amount, 0..1; set from Param6
    float ampEnv = 1.f;     // note-on declick ramp, 0 (silent) -> 1 (full level)
    MoogLadder ladder;
  };

  State s_state;

  // --- Fixed placeholder. Left as a hardcoded constant for now; see the
  //     project README. --
  constexpr float k_cutoffMinHz = 60.f;    // SHAPE = 0   -> filter fully closed (dark/thumpy)
  constexpr float k_cutoffMaxHz = 7000.f;  // SHAPE = max -> filter fully open (bright/buzzy)

  // SHIFT+SHAPE -> resonance range. ~4.0 is where this filter model starts
  // to self-oscillate; capped a bit under that (rather than mapping all the
  // way to it) so the full knob travel stays a controllable growl instead
  // of the last few percent suddenly screaming into feedback.
  constexpr float k_resonanceMax = 3.8f;

  // Osc1/Osc2 Detune params sweep +/- this many cents. 50 cents (a quarter
  // tone) per oscillator means the two can spread up to a full semitone
  // apart at opposite extremes -- enough for an obvious "fat/wide" unison
  // effect while still sounding like one note, not a chord.
  constexpr float k_maxDetuneCents = 50.f;

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
  // phase increments are recomputed once per block rather than once per
  // note. w0 is the note's true, undetuned pitch -- the sub-oscillator
  // always tracks this directly, one octave down, regardless of what Osc1
  // or Osc2's detune is doing.
  const float w0 = osc_w0f_for_note((params->pitch) >> 8, params->pitch & 0xFF);
  const float wSub = w0 * 0.5f; // one octave below the root pitch

  // Detune expressed as a frequency ratio: 2^(cents/1200). fastpow2f is
  // this SDK's fast approximation of 2^x.
  const float w1 = w0 * fastpow2f(s_state.osc1DetuneCents * (1.f / 1200.f));
  const float w2 = w0 * fastpow2f(s_state.osc2DetuneCents * (1.f / 1200.f));

  float phase1 = s_state.phase1;
  float phase2 = s_state.phase2;
  float phaseSub = s_state.phaseSub;
  float ampEnv = s_state.ampEnv;
  const float cutoffNorm = s_state.cutoffNorm;
  const float resonance = s_state.resonance;
  const float osc1Shape = s_state.osc1Shape;
  const float osc2Shape = s_state.osc2Shape;
  const float blend = s_state.blend;
  const float subLevel = s_state.subLevel;
  MoogLadder &ladder = s_state.ladder;

  q31_t * __restrict y = (q31_t *)yn;
  const q31_t * const y_end = y + frames;

  for (; y != y_end; ++y) {
    // Each oscillator crossfades from sawtooth (shape=0) to square
    // (shape=1) independently. A different waveform shape at the source
    // changes the harmonic content feeding the filter -- saw is
    // bright/buzzy with every harmonic present, square is hollower/woodier
    // with only odd harmonics.
    const float osc1 = (1.f - osc1Shape) * blep_saw(phase1, w1) + osc1Shape * blep_square(phase1, w1);
    const float osc2 = (1.f - osc2Shape) * blep_saw(phase2, w2) + osc2Shape * blep_square(phase2, w2);

    // Osc1/Osc2 crossfade, not a sum -- this is what keeps "Blend" from
    // needing two separate volume knobs, and it's why detuning them apart
    // creates movement: at blend=0.5 you're constantly fading between two
    // waveforms that are very slightly out of sync with each other.
    const float oscMix = (1.f - blend) * osc1 + blend * osc2;

    const float sub = blep_square(phaseSub, wSub);

    float raw = (1.f - subLevel) * oscMix + subLevel * sub;
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
    const float filtered = ladder.process(raw, cutoffNorm, resonance);

    *y = f32_to_q31(clip1m1f(filtered));

    phase1 += w1;
    phase1 -= (uint32_t)phase1;
    phase2 += w2;
    phase2 -= (uint32_t)phase2;
    phaseSub += wSub;
    phaseSub -= (uint32_t)phaseSub;
  }

  s_state.phase1 = phase1;
  s_state.phase2 = phase2;
  s_state.phaseSub = phaseSub;
  s_state.ampEnv = ampEnv;
}

void OSC_NOTEON(const user_osc_param_t * const params)
{
  (void)params;
  // Restart all three oscillator phases in sync for a consistent attack on
  // every note (they'll drift apart over the note's sustain if detuned --
  // that's the intended "movement" effect, just not on the very first
  // sample), and re-arm the declick ramp (see k_declickInc) to hide the
  // resulting amplitude jump. The filter's memory is deliberately left
  // alone -- see the comment in OSC_CYCLE.
  s_state.phase1 = 0.f;
  s_state.phase2 = 0.f;
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
  case k_user_osc_param_shiftshape: {
    // SHIFT+SHAPE knob (10-bit: 0..1023) -> filter resonance. Linear
    // mapping is fine here (unlike cutoff, resonance isn't a frequency, so
    // there's no perceptual reason to curve it) -- 0 = clean, k_resonanceMax
    // = an edgy near-self-oscillating growl.
    const float shiftshape01 = param_val_to_f32(value); // 0..1
    s_state.resonance = shiftshape01 * k_resonanceMax;
    break;
  }
  case k_user_osc_param_id1:
    // Param1 "O1 Shape" (0-100%) -> Osc1 saw->square blend.
    s_state.osc1Shape = clip01f(value * 0.01f);
    break;
  case k_user_osc_param_id2: {
    // Param2 "O1 Detune" -- bipolar percent. Per the minilogue xd SDK's
    // convention for bipolar params, the raw value arrives as 0..200 with
    // 100 = center (0%), not as the signed -100..100 shown in the manifest.
    const float pct = (float)((int16_t)value - 100) * 0.01f; // -1..1
    s_state.osc1DetuneCents = pct * k_maxDetuneCents;
    break;
  }
  case k_user_osc_param_id3:
    // Param3 "O2 Shape" (0-100%) -> Osc2 saw->square blend.
    s_state.osc2Shape = clip01f(value * 0.01f);
    break;
  case k_user_osc_param_id4: {
    // Param4 "O2 Detune" -- bipolar percent, same convention as O1 Detune.
    const float pct = (float)((int16_t)value - 100) * 0.01f; // -1..1
    s_state.osc2DetuneCents = pct * k_maxDetuneCents;
    break;
  }
  case k_user_osc_param_id5:
    // Param5 "Blend" (0-100%) -> Osc1<->Osc2 crossfade.
    s_state.blend = clip01f(value * 0.01f);
    break;
  case k_user_osc_param_id6:
    // Param6 "Sub Mix" (0-100%) -> sub-oscillator mix amount.
    s_state.subLevel = clip01f(value * 0.01f);
    break;
  default:
    break;
  }
}
