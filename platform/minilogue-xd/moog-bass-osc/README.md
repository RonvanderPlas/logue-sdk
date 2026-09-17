# moog-bass-osc

A custom minilogue xd user oscillator aiming for a Moog-style monophonic
bass voice. Implementation lives in [`moogbass.cpp`](./moogbass.cpp); this
file explains what it does and why, at a level above the code comments.

## Signal flow

```
[ Osc1: saw<->square, detuned ] --+
                                   +--> [ blend ] --+
[ Osc2: saw<->square, detuned ] --+                +--> [ mix ] --> [ 4-pole ladder filter ] --> out
                                                     |        ^           ^
[ sub-osc, -1 oct, root pitch ] --------------------+  SHAPE = cutoff     |
                                                                SHIFT+SHAPE = resonance
```

1. **Osc1 / Osc2** — two independent bandlimited (PolyBLEP) oscillators,
   each morphing from sawtooth to square (its own "Shape" param) and each
   with its own pitch offset in cents ("Detune"). They're crossfaded
   together by **Blend** (0% = only Osc1, 100% = only Osc2, 50% = equal
   parts of both) rather than mixed with two separate volume knobs — see
   "Why a blend, not two volumes" below.
2. **Sub-oscillator** — a square wave exactly one octave below the note's
   true pitch. It always tracks the root note directly, unaffected by
   either oscillator's detune, and mixed in underneath via **Sub Mix** to
   add low-end weight (a common trick on real analog bass patches).
3. **4-pole ladder filter** — a digital model of the classic Moog transistor
   ladder (24 dB/octave lowpass with resonance/feedback). This is what gives
   the sound its "Moog" character, and it's what **SHAPE** and **SHIFT+SHAPE**
   control together.

### Why a blend, not two volumes

Detuning Osc1 and Osc2 slightly apart from each other is what makes the
combined sound feel "fat" or "wide" — the two waveforms are never quite in
sync, so their peaks and zero-crossings constantly drift in and out of
alignment, which the ear hears as movement rather than a single static
tone. A single crossfade knob (rather than independent Osc1/Osc2 volumes)
keeps that effect a one-knob control: at 50% you always hear both in equal
measure, and sweeping the knob shifts which oscillator dominates without
also changing the overall loudness.

## What the knobs do

- **SHAPE** → filter cutoff frequency, mapped exponentially from 60 Hz
  (fully closed, dark/thumpy) to 7 kHz (fully open, bright/buzzy). An
  exponential curve is used because pitch/frequency perception is
  logarithmic — it makes the knob's sweep feel even across its whole travel,
  rather than all the action being bunched up at one end.
- **SHIFT+SHAPE** → filter resonance, mapped linearly from 0 (clean) to
  `k_resonanceMax` (`3.8`, an edgy near-self-oscillating growl). Linear is
  fine here since, unlike cutoff, resonance isn't a frequency — there's no
  perceptual reason to curve it. Together, SHAPE and SHIFT+SHAPE behave like
  the cutoff and resonance knobs on a Minimoog's filter section.
- **Param1 "O1 Shape"** / **Param3 "O2 Shape"** → each oscillator's own
  0–100% linear crossfade from sawtooth to square.
- **Param2 "O1 Detune"** / **Param4 "O2 Detune"** → each oscillator's pitch
  offset, bipolar (-100%..+100%), scaled to +/- `k_maxDetuneCents` (50
  cents) — so at opposite extremes the two oscillators can spread up to a
  full semitone apart. These are the SDK's "bipolar percent" params: the
  manifest shows -100..100, but the raw value handed to the code is
  actually 0..200 with 100 = center — see the conversion in `OSC_PARAM`.
- **Param5 "Blend"** → 0–100% crossfade between Osc1 (0%) and Osc2 (100%).
- **Param6 "Sub Mix"** → sub-oscillator mix amount, 0–100%, linear.

All six Param slots are now in use. Defaults are whatever the panel/patch
already has them set to — after this update, existing saved patches will
have their Param1–6 knob positions reinterpreted for these new roles (the
params were reordered/expanded), so expect to redial the sound in.

## What's still a fixed placeholder

The cutoff knob's **Hz range** (60 Hz–7 kHz, see `k_cutoffMinHz`/
`k_cutoffMaxHz` in `moogbass.cpp`) is still a hardcoded constant. Left as-is
for now since it already covers useful bass territory — worth revisiting if
you want it darker/brighter at the extremes.

## Known limitations / open questions

- No oversampling: the ladder filter runs at the native 48 kHz sample rate.
  Now that resonance is a real, live-playable knob (up to `3.8`, near
  self-oscillation), pushing it hard at a bright cutoff is the most likely
  way to hear aliasing/harshness creep in. If that turns out to be
  audible in practice, 2x oversampling around the filter is the fix.
- Phase resets on every note-on (see `OSC_NOTEON`), which gives a
  consistent, punchy attack transient but no "free-running" drift between
  notes. To avoid an audible click from that reset, note-on re-arms a short
  (2ms) amplitude ramp (`k_declickInc`) and, importantly, does **not** clear
  the filter's internal memory — resetting a resonant filter's state right
  before feeding it a fresh signal causes an audible "knock" as it
  resettles, worst at low cutoff where the filter is slowest to settle.
  (Found and fixed after hearing exactly this click on hardware.)

## Building

From the repo root, using the Docker build environment:

```
docker/run_cmd.sh build minilogue-xd/moog-bass-osc
```

This produces `moog_bass_osc.mnlgxdunit` in this directory, ready to load
onto the synth via the Korg Librarian / logue-cli.
