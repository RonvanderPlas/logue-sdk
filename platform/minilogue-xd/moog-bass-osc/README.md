# moog-bass-osc

A custom minilogue xd user oscillator aiming for a Moog-style monophonic
bass voice. Implementation lives in [`moogbass.cpp`](./moogbass.cpp); this
file explains what it does and why, at a level above the code comments.

## Signal flow

```
[ bandlimited saw ] --+
                       +--> [ mix ] --> [ 4-pole ladder filter ] --> out
[ sub-osc, -1 oct  ] --+                        ^
                                          SHAPE knob = cutoff
```

1. **Sawtooth oscillator** — the primary tone source, generated with the
   PolyBLEP technique so the waveform's edge doesn't alias at low bass notes.
2. **Sub-oscillator** — a square wave exactly one octave below the saw,
   mixed in underneath it to add low-end weight (a common trick on real
   analog bass patches).
3. **4-pole ladder filter** — a digital model of the classic Moog transistor
   ladder (24 dB/octave lowpass with resonance/feedback). This is what gives
   the sound its "Moog" character, and it's what the **SHAPE knob** controls.

## What the knob does

- **SHAPE** → filter cutoff frequency, mapped exponentially from 60 Hz
  (fully closed, dark/thumpy) to 7 kHz (fully open, bright/buzzy). An
  exponential curve is used because pitch/frequency perception is
  logarithmic — it makes the knob's sweep feel even across its whole travel,
  rather than all the action being bunched up at one end.
- **SHIFT+SHAPE, Param 1–6** — not wired up yet.

## What's still a fixed placeholder

These are hardcoded constants at the top of `moogbass.cpp` for now, to be
turned into real params once the core sound is dialed in (see the constants
`k_subLevel`, `k_resonance`, `k_cutoffMinHz`, `k_cutoffMaxHz`):

- Filter **resonance** (currently fixed at a mild growl, `1.4` out of ~4
  where the filter starts to self-oscillate)
- **Sub-oscillator level** (currently fixed at 35% mix)
- The cutoff knob's **Hz range** (currently 60 Hz–7 kHz)

## Known limitations / open questions

- No oversampling: the ladder filter runs at the native 48 kHz sample rate.
  This is fine at the current cutoff range, but if resonance is pushed
  higher later (towards self-oscillation) it may need 2x oversampling to
  stay alias-free — worth revisiting once resonance becomes a real knob.
- Phase resets on every note-on (see `OSC_NOTEON`), which gives a
  consistent, punchy attack transient but no "free-running" drift between
  notes.

## Building

From the repo root, using the Docker build environment:

```
docker/run_cmd.sh build minilogue-xd/moog-bass-osc
```

This produces `moog_bass_osc.mnlgxdunit` in this directory, ready to load
onto the synth via the Korg Librarian / logue-cli.
