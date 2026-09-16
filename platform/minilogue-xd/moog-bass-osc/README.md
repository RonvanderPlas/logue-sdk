# moog-bass-osc

A custom minilogue xd user oscillator aiming for a Moog-style monophonic
bass voice. Implementation lives in [`moogbass.cpp`](./moogbass.cpp); this
file explains what it does and why, at a level above the code comments.

## Signal flow

```
[ bandlimited saw ] --+
                       +--> [ mix ] --> [ 4-pole ladder filter ] --> out
[ sub-osc, -1 oct  ] --+                    ^           ^
                                      SHAPE = cutoff     |
                                               SHIFT+SHAPE = resonance
```

1. **Sawtooth oscillator** — the primary tone source, generated with the
   PolyBLEP technique so the waveform's edge doesn't alias at low bass notes.
2. **Sub-oscillator** — a square wave exactly one octave below the saw,
   mixed in underneath it to add low-end weight (a common trick on real
   analog bass patches).
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
- **Param 1–6** — not wired up yet.

## What's still a fixed placeholder

These are hardcoded constants at the top of `moogbass.cpp` for now, to be
turned into real Param 1–6 params once the core sound is dialed in (see the
constants `k_subLevel`, `k_cutoffMinHz`, `k_cutoffMaxHz`):

- **Sub-oscillator level** (currently fixed at 35% mix)
- The cutoff knob's **Hz range** (currently 60 Hz–7 kHz)

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
