// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "DspUtils.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace drum
{

/** The output clipper, the last stage of the master chain. Modes are the
    indices of clipModeNames:

      Off   no clipper and no drive: the input comes out untouched, only
            delayed by the latency (it can go over 0 dBFS; the host decides).
      Soft  unchanged below a -1 dB knee (relative to the ceiling), tanh above.
            At 0 dB drive and a 0 dBFS ceiling this is the original soft clipper.
      Hard  a hard clip at the ceiling.
      Tube  asymmetric: tanh plus a squared term, like a triode, so the 2nd
            harmonic grows with level (roughly -25 dB at half the ceiling). The
            positive half flattens out at 2/3 of the ceiling, the negative at
            the ceiling. The even part (the only part that can carry DC) goes
            through a 10 Hz DC blocker, like a coupling capacitor.
      Tape  a gentle arctan / cubic blend with a very soft knee, wrapped in a
            pre-emphasis / de-emphasis pair (+4 dB shelf above ~5 kHz and its
            exact inverse), so the pair cancels at low levels, but when driven
            the highs saturate first and come out softened.
      Fold  unchanged below 0.8 x the ceiling, then a sine that peaks at the
            ceiling and keeps folding (a cosine) the harder it's pushed.

    Drive (0..24 dB) is gain into the shaper; nothing makes it up afterwards,
    so a hotter drive clips harder while the output stays at the ceiling
    (below the knee it simply makes things louder). Ceiling (-12..0 dBFS)
    scales the curve: out = ceiling * shape (drive * in / ceiling).

    Anti-aliasing: the shaping runs at 8x (4x from 88.2 kHz, 2x from 176.4
    kHz) with linear-phase Kaiser-windowed FIR interpolation and decimation
    (70 dB stopband; the passband runs to 0.4 x the sample rate or 20 kHz).
    Only the difference the shaper makes (shape(u) - u) goes through the
    oversampler; it is added to the input delayed by the same, whole number of
    samples. Below the knee that difference is exactly zero, so Soft at its
    defaults and Off are bit-for-bit the delayed input, and only the samples
    that clip change.

    Band-limiting a clipped waveform overshoots (Gibbs; a hard-clipped sine's
    fundamental alone is 4/pi of the ceiling), so a final look-ahead peak
    limiter (0.2 ms linked attack, 30 ms release, then a clamp for rounding)
    keeps every sample at or below the ceiling. It sits at unity, and so is
    bit-transparent, unless the band-limited output actually goes over. It is
    off in Off, and eases in and out over a crossfade to or from Off.

    The latency (FIR delay + look-ahead) depends only on the sample rate, never
    on the mode. Drive and ceiling glide over 20 ms; switching modes crossfades
    over 10 ms (a switch that arrives mid-fade waits for the fade to end).

    prepare() allocates; process() doesn't allocate or lock. */
class Clipper
{
public:
    enum Mode { off = 0, soft, hard, tube, tape, fold, numModes };

    /** The static (1x, settled) transfer curve out = f (in) for the UI:
        mode indexes clipModeNames. Tube leaves out its DC blocker and Tape its
        emphasis filters (both are neutral at low frequencies). Off returns x.
        Any thread. */
    static float transfer (int mode, float x, float driveDb, float ceilingDb) noexcept;

    /** Oversampling factor used at a given sample rate (8, 4, 2 or 1). */
    static int factorForSampleRate (double sampleRate) noexcept;

    void prepare (double sampleRate);
    /** Clears all state; the next block starts from its own settings without
        gliding or fading. */
    void reset() noexcept;

    /** Constant for a given sample rate, whatever the mode. */
    int getLatencySamples() const noexcept { return latency; }
    int getOversamplingFactor() const noexcept { return factor; }

    /** Processes a stereo pair in place. Any block size. */
    void process (float* left, float* right, int numSamples,
                  int mode, float driveDb, float ceilingDb) noexcept;

private:
    /** Mirrored ring buffer: the last n values are always contiguous. */
    struct Ring
    {
        std::vector<float> data;
        int size = 1, pos = 0;

        void setSize (int minSize);
        void clear() noexcept;
        void push (float v) noexcept
        {
            data[(size_t) pos] = data[(size_t) (pos + size)] = v;
            pos = (pos + 1) & (size - 1);
        }
        /** The last n values, oldest first. n <= size. */
        const float* last (int n) const noexcept { return data.data() + pos + size - n; }
        /** The value pushed d pushes ago (0 = the newest). */
        float delayed (int d) const noexcept { return data[(size_t) (pos + size - 1 - d)]; }
    };

    /** First-order IIR section (direct form I), one channel. */
    struct FirstOrder
    {
        float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;

        void reset() noexcept { x1 = y1 = 0.0f; }
        float process (float x) noexcept
        {
            const float y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    bool usesMode (int m) const noexcept { return mode == m || (fading && prevMode == m); }
    void startMode (int m) noexcept;
    float residual (int m, float u, float gain, float ceiling, int ch) noexcept;
    bool belowKnee (float gain, float ceiling) const noexcept;

    double sampleRate = 44100.0;
    int factor = 1, taps = 1, phaseTaps = 1, firDelay = 0, lookahead = 1, latency = 0;
    std::vector<float> fir;       // taps, symmetric, unity DC gain (the decimator)
    std::vector<float> phases;    // factor x phaseTaps, time-reversed and x factor (the interpolator)
    float phaseGain = 1.0f;       // bound on |interpolated| / max |input|

    Ring in[2], diff[2], pre[2], need, held, limit;
    int zeroRun = 0;              // oversampled samples since diff[] last held a non-zero value

    juce::SmoothedValue<float> drive, ceiling;
    int mode = soft, prevMode = soft, fadePos = 0, fadeLength = 1;
    bool fading = false, first = true;

    float limGain = 1.0f, releaseCoef = 0.0f;

    dsp::DcBlocker tubeDc[2];
    FirstOrder tapePre[2], tapeDe[2];
};

} // namespace drum
