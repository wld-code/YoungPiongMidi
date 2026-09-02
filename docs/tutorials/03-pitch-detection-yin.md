[← Tutorial 2](02-dsp-fundamentals.md) · [Tutorials index](README.md)

# Tutorial 3: Pitch detection with YIN

**Code**: `components/pitch/yin.c`

This is the trickiest part of the whole pipeline conceptually, so this
tutorial takes it in small steps: what "pitch" even means for a
non-mathematician, why the obvious approach (FFT) isn't used here, and
then how YIN actually works, one step at a time.

## What "pitch" means, physically

A musical note - a sung "ahh," a plucked string - is (approximately) a
**periodic** wave: a pattern that repeats over and over. The **period**
is how long one repetition takes; the **fundamental frequency** (what we
call "pitch") is just `1 / period`, in cycles per second (Hz).

```
amplitude
   ^
   |   /\        /\        /\        /\
   |  /  \      /  \      /  \      /  \
   | /    \    /    \    /    \    /    \
   +/------\--/------\--/------\--/------\--> time
   |        \/        \/        \/
              <--period-->
```

A440 (concert A) means the wave repeats 440 times per second - a period
of `1/440 ≈ 2.27ms`. At `YP_AUDIO_SAMPLE_RATE_HZ = 16000`, that's about
36 samples per period.

## Why not FFT?

The FFT (Fast Fourier Transform) is the classic tool for finding
frequency content in a signal, and it might seem like the obvious choice
here. The problem is **resolution**: an FFT run on `N` samples splits the
frequency axis into `N/2` equally-spaced bins, each
`sample_rate / N` Hz wide. With a 512-sample window at 16kHz, each bin
is about 31 Hz wide - but a semitone near A2 (110 Hz) is only about
6.5 Hz wide. An FFT alone can't tell A2 from a note a third of a
semitone away; you'd need extra interpolation tricks on top just to get
musically useful precision, and even then, picking the "loudest" bin as
the pitch (a common naive approach) gets confused by a human voice's
**harmonics** - the pitch you perceive is usually the fundamental, but
it's often not even the loudest partial in the spectrum.

YIN is a **time-domain** method (it works on the waveform directly, not
a spectrum) purpose-built for monophonic pitch tracking, and it
naturally gives much finer precision. It's the method the project spec
asks for by name.

## Step 1: the difference function - "does this look like a copy of itself, shifted?"

The core idea behind YIN (and its ancestor, **autocorrelation**): if a
signal has period `T`, then shifting it by exactly `T` samples and
comparing it to the original should show almost no difference - a
periodic wave laid on top of a `T`-shifted copy of itself lines up.

```
original:   /\    /\    /\    /\
shifted by T:  /\    /\    /\    /\
              (near-perfect overlap: this IS the period)

original:   /\    /\    /\    /\
shifted by T/2: \/    \/    \/    \/
              (bad overlap: half a period is NOT the period)
```

YIN's **difference function** measures exactly this "how different does
it look" for every candidate shift (**lag**) `tau`:

```
d(tau) = sum over a window of (x[j] - x[j + tau])^2
```

Small `d(tau)` means "this lag looks like the true period or a multiple
of it." The candidate `tau` search only needs to cover the range of lags
corresponding to musically plausible pitches -
`YP_PITCH_MIN_HZ`/`YP_PITCH_MAX_HZ` (80-1000 Hz) translate directly into
a lag range via `tau = sample_rate / frequency`.

## Step 2: normalize it (CMNDF)

Raw `d(tau)` naturally grows as `tau` grows (you're summing over more
implicit "stuff"), which makes small `d(tau)` values at different `tau`
hard to compare directly. YIN's fix is the **cumulative mean normalized
difference function** - divide `d(tau)` by its own running average up to
that point:

```
d'(tau) = d(tau) * tau / (running sum of d(1..tau))
d'(0) is defined as 1 (so lag 0, trivially identical to itself,
                        is never mistaken for "the" period)
```

This flattens the trend so different `tau` values become genuinely
comparable, and gives a value that behaves like "how confident are we
this is periodic here" - close to 0 is very periodic, close to 1 is not.

## Step 3: pick a lag - and avoid the octave trap

The simplest thing to do now would be "pick the `tau` with the smallest
`d'(tau)`" - but this has a famous failure mode: if the true period is
`T`, then `2T`, `3T`, etc. also look pretty periodic (two full cycles of
a wave still line up with a two-cycle shift), sometimes even *slightly*
better than `T` itself due to noise. Picking the global minimum can lock
onto an **octave** below the real pitch.

YIN's fix is the **absolute threshold** rule: walk `tau` from small to
large, and take the *first* dip that goes below a threshold
(`YP_PITCH_YIN_THRESHOLD = 0.15`) - not the deepest dip anywhere. Once
you're below threshold, keep going while it keeps improving (a **local
minimum**), then stop:

```
d'(tau)
  1.0 |*
      | *
      |  *   .            .
      |   * . .          . .
 0.15 |----*---*--------*---*----  <- threshold
      |     *.*          *.*
  0.0 +------#------------#------> tau (lag)
             ^first dip    ^deeper dip further out (an octave below -
              below         correctly ignored because we already
              threshold      committed to the first one)
```

This is *the* idea that makes YIN specifically good for monophonic pitch
tracking, as opposed to a naive "find the global minimum" autocorrelation
search.

## Step 4: sub-sample precision (parabolic interpolation)

`tau` so far is a whole number of samples. At 16kHz, going from `tau=182`
to `tau=183` near 88 Hz is already about a 9-cent jump (100 cents = one
semitone) - too coarse for anything musically convincing. YIN fits a
parabola through the chosen `tau` and its two neighbors, and uses the
parabola's minimum (which can land *between* samples) as the final,
fractional lag:

```
d'(tau)
    |   *                    *  <- neighbor
    |    \                  /
    |     \   fitted       /
    |      \  parabola    /
    |       \............/
    |        *
    |     true minimum, between two sample points
    +-------------------------------> tau
```

`interpolate()` in `yin.c` does exactly this, and `confidence` in the
final result is `1 - d'(tau)` at that refined point - directly usable
downstream (see [Tutorial 5](05-note-stabilization.md)) to decide
whether a pitch estimate is trustworthy enough to act on.

## A real engineering detour: no floating-point hardware

Everything above is naturally described with real (fractional) numbers.
The obvious C implementation uses `float` throughout. On most modern
microcontrollers that would be fine - but ESP32-C5 is a RISC-V chip
built *without* a hardware floating-point unit (FPU). Every `float`
operation is emulated in software, at a real cost: the first all-`float`
version of this exact algorithm measured **~79ms** to analyze one hop,
against an 8ms budget - roughly 10x too slow, discovered by *measuring
on real hardware*, not by guessing.

The fix: `yin.c`'s difference-function inner loop (by far the most
frequently-executed part - it runs for every sample, for every
candidate `tau`) works in **fixed-point** integer arithmetic instead.
The idea behind fixed-point: instead of a `float` tracking its own
decimal point, you pick a scale up front (here, `Q14`: multiply every
real value by `16384` and store it as an integer) and do ordinary
integer math, which this chip's hardware handles natively and fast:

```
real value 0.5  -> stored as integer 8192   (0.5 * 16384)
real value -0.25 -> stored as integer -4096  (-0.25 * 16384)
```

Only the parts of the algorithm that run *once per hop*, not once per
sample-and-lag (the normalization, threshold search, and interpolation
above), stay as `float` - there, soft-float's cost is negligible. This
cut the measured cost to ~13ms, and a further change (explained in
[Tutorial 6](06-freertos-realtime.md), where it fits naturally alongside
other real-time scheduling decisions) brought the *typical* cost down to
around 4.7ms per hop. See `docs/tuning.md` for the full measured story.

## What comes out of this stage

`pitch_detector_process()` returns `{frequency_hz, confidence}`, merged
into the same `voice_analysis_t` that [Tutorial 2](02-dsp-fundamentals.md)
produces `rms`/`level`/`voice_active` into. From here,
[Tutorial 4](04-midi-basics.md) covers what a frequency in Hz actually
*means* as a musical note.

---
**Previous:** [← Tutorial 2 - DSP fundamentals](02-dsp-fundamentals.md)
**Next:** [Tutorial 4 - MIDI basics →](04-midi-basics.md)
