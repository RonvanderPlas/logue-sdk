# moog-bass-osc

A custom minilogue xd user oscillator aiming for a Moog-style monophonic
bass voice. Implementation lives in [`moogbass.cpp`](./moogbass.cpp); this
file explains what it does and why, at a level above the code comments.

## Signal flow

```
[ saw <-> square ] --+
                       +--> [ mix ] --> [ 4-pole ladder filter ] --> out
[ sub-osc, -1 oct  ] --+   ^   ^            ^           ^
            Param2 = Osc Shape |      SHAPE = cutoff     |
                  Param1 = Sub Mix            SHIFT+SHAPE = resonance
```

1. **Primary oscillator** — a bandlimited (PolyBLEP) sawtooth by default,
   morphing towards a square wave as "Osc Shape" increases. Saw is
   bright/buzzy with every harmonic present; square is hollower/woodier
   with only odd harmonics — genuinely different oscillator colors, not
   just a filter setting.
2. **Sub-oscillator** — a square wave exactly one octave below the primary
   oscillator (independent of Osc Shape — always a plain octave-down
   square), mixed in underneath to add low-end weight (a common trick on
   real analog bass patches).
3. **4-pole ladder filter** — a digital model of the classic Moog transistor
   ladder (24 dB/octave lowpass with resonance/feedback). This is what gives
   the sound its "Moog" character, and it's what **SHAPE** and **SHIFT+SHAPE**
   control together.

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
- **Param1 "Sub Mix"** → sub-oscillator mix amount, 0–100%, linear. Default
  is whatever the panel/patch has it set to (likely 0% until you dial it in
  for the first time).
- **Param2 "Osc Shape"** → primary oscillator waveform, 0–100%, linear
  crossfade from sawtooth (0%) to square (100%). This only affects the
  primary oscillator, not the sub.
- **Param 3–6** — not wired up yet.

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
