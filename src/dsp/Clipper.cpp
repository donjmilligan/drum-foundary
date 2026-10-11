// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "Clipper.h"

namespace drum
{

namespace
{
    constexpr float kSoftKnee = 0.891f;     // -1 dB (x ceiling); the original soft clipper's knee
    constexpr float kFoldKnee = 0.8f;       // Fold is straight up to here
    constexpr double kStopbandDb = 70.0;    // FIR interpolator / decimator
    constexpr double kFadeSeconds = 0.01;
    constexpr double kLookaheadSeconds = 0.0002;
    constexpr double kReleaseSeconds = 0.03;
    constexpr double kSmoothSeconds = 0.02;
    constexpr float kTapeEmphasisDb = 4.0f;
    constexpr double kTapeEmphasisHz = 7000.0; // pole of the shelf; its zero is ~4.5 kHz

    /** The original soft clipper, ceiling 1. Exactly the same arithmetic. */
    inline float softUnit (float x) noexcept
    {
        const float a = std::abs (x);
        if (a <= kSoftKnee)
            return x;
        const float y = kSoftKnee + (1.0f - kSoftKnee) * std::tanh ((a - kSoftKnee) / (1.0f - kSoftKnee));
        return std::copysign (y, x);
    }

    inline float hardUnit (float x) noexcept { return juce::jlimit (-1.0f, 1.0f, x); }

    /** Tube: tanh plus a squared (even) term that lifts the negative half and
        flattens the positive one, like a triode's 2nd harmonic, which grows
        with level. Scaled so the slope at 0 is 1 and the curve stays within
        [-1, (1 - a) / (1 + a)]; the even part is returned separately so it can
        go through the DC blocker. */
    constexpr float kTubeEven = 0.2f;
    struct TubeParts { float odd, even; };
    inline TubeParts tubeParts (float x) noexcept
    {
        constexpr float q = 1.0f + kTubeEven;
        const float t = std::tanh (q * x);
        return { t * (1.0f / q), -kTubeEven / q * t * t };
    }

    inline float tubeUnit (float x) noexcept
    {
        const auto t = tubeParts (x);
        return t.odd + t.even;
    }

    /** Half arctan, half cubic soft clip (the cubic reaches 1 at 1.5 and stays). */
    inline float tapeUnit (float x) noexcept
    {
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        const float arc = std::atan (halfPi * x) * (1.0f / halfPi);
        const float cubic = std::abs (x) < 1.5f ? x - (4.0f / 27.0f) * x * x * x : std::copysign (1.0f, x);
        return 0.5f * (arc + cubic);
    }

    /** Straight to the knee, then a sine that peaks at 1 and a cosine that keeps
        folding between -1 and 1, matched in slope and curvature at each join. */
    inline float foldUnit (float x) noexcept
    {
        constexpr float span = 1.0f - kFoldKnee;
        constexpr float peakAt = kFoldKnee + span * juce::MathConstants<float>::halfPi;
        const float a = std::abs (x);
        if (a <= kFoldKnee)
            return x;
        const float y = a <= peakAt ? kFoldKnee + span * std::sin ((a - kFoldKnee) * (1.0f / span))
                                    : std::cos ((a - peakAt) * 2.2360680f); // 1 / sqrt (span)
        return std::copysign (y, x);
    }

    /** Where each mode stops being a straight line (for skipping the work). */
    inline float linearUpTo (int mode) noexcept
    {
        switch (mode)
        {
            case Clipper::soft: return kSoftKnee;
            case Clipper::hard: return 1.0f;
            case Clipper::fold: return kFoldKnee;
            default:            return 0.0f;
        }
    }

    inline float dot (const float* a, const float* b, int n) noexcept
    {
        // n is a multiple of 4; four accumulators let this vectorise.
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        for (int i = 0; i < n; i += 4)
        {
            s0 += a[i] * b[i];
            s1 += a[i + 1] * b[i + 1];
            s2 += a[i + 2] * b[i + 2];
            s3 += a[i + 3] * b[i + 3];
        }
        return (s0 + s1) + (s2 + s3);
    }

    inline int roundUp4 (int n) noexcept { return (n + 3) & ~3; }

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            const double f = x / (2.0 * k);
            term *= f * f;
            sum += term;
            if (term < 1.0e-14 * sum)
                break;
        }
        return sum;
    }

    float sanitise (float v, float lo, float hi, float fallback) noexcept
    {
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
    }
}

//==============================================================================
float Clipper::transfer (int m, float x, float driveDb, float ceilingDb) noexcept
{
    if (m <= off || m >= numModes)
        return x;

    const float c = juce::Decibels::decibelsToGain (sanitise (ceilingDb, -12.0f, 0.0f, 0.0f));
    const float v = x * (juce::Decibels::decibelsToGain (sanitise (driveDb, 0.0f, 24.0f, 0.0f)) / c);
    switch (m)
    {
        case soft: return c * softUnit (v);
        case hard: return c * hardUnit (v);
        case tube: return c * tubeUnit (v);
        case tape: return c * tapeUnit (v);
        case fold:
        default:   return c * foldUnit (v);
    }
}

int Clipper::factorForSampleRate (double sr) noexcept
{
    return sr < 50000.0 ? 8 : sr < 100000.0 ? 4 : sr < 200000.0 ? 2 : 1;
}

void Clipper::Ring::setSize (int minSize)
{
    size = juce::nextPowerOfTwo (juce::jmax (2, minSize));
    data.assign ((size_t) size * 2, 0.0f);
    pos = 0;
}

void Clipper::Ring::clear() noexcept
{
    std::fill (data.begin(), data.end(), 0.0f);
    pos = 0;
}

void Clipper::prepare (double sr)
{
    sampleRate = sr;
    factor = factorForSampleRate (sr);
    const double osRate = sr * factor;

    // --- Linear-phase lowpass at the oversampled rate, cut off at the base
    //     Nyquist. Its transition runs from the passband edge to its mirror
    //     image, so anything that folds back lands above the passband. The
    //     length makes the round trip (N - 1 samples) a whole number of base
    //     samples: firDelay.
    std::vector<double> h { 1.0 };
    firDelay = 0;
    if (factor > 1)
    {
        const double pass = juce::jmin (20000.0, 0.4 * sr);
        const double transition = juce::MathConstants<double>::twoPi * (sr - 2.0 * pass) / osRate;
        const double order = (kStopbandDb - 8.0) / (2.285 * transition);
        firDelay = (int) std::ceil (order / factor);
        const int n = factor * firDelay + 1;
        const double beta = 0.1102 * (kStopbandDb - 8.7);
        const double centre = 0.5 * (n - 1);
        h.assign ((size_t) n, 0.0);
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (i - centre) / factor;
            const double sinc = juce::exactlyEqual (t, 0.0) ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const double r = (i - centre) / centre;
            h[(size_t) i] = sinc * besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / besselI0 (beta);
            sum += h[(size_t) i];
        }
        for (auto& c : h)
            c /= sum;
    }

    const int n = (int) h.size();
    taps = roundUp4 (n);
    fir.assign ((size_t) taps, 0.0f);          // zero-padded at the oldest end
    for (int i = 0; i < n; ++i)
        fir[(size_t) (taps - n + i)] = (float) h[(size_t) i];

    // Polyphase interpolator: phase p makes output sample R*m + p from the last
    // phaseTaps inputs (oldest first), with gain R to make up for the zeros.
    const int perPhase = (n + factor - 1) / factor;
    phaseTaps = roundUp4 (perPhase);
    phases.assign ((size_t) (factor * phaseTaps), 0.0f);
    phaseGain = 0.0f;
    for (int p = 0; p < factor; ++p)
    {
        float absSum = 0.0f;
        for (int j = 0; j < perPhase; ++j)
            if (const int i = p + factor * j; i < n)
            {
                const float c = (float) (h[(size_t) i] * factor);
                phases[(size_t) (p * phaseTaps + phaseTaps - 1 - j)] = c;
                absSum += std::abs (c);
            }
        phaseGain = juce::jmax (phaseGain, absSum);
    }
    phaseGain *= 1.001f; // margin for rounding in the dot product

    lookahead = juce::jmax (1, (int) std::lround (kLookaheadSeconds * sr));
    latency = firDelay + lookahead - 1;
    releaseCoef = (float) (1.0 - std::exp (-1.0 / (kReleaseSeconds * sr)));
    fadeLength = juce::jmax (1, (int) std::lround (kFadeSeconds * osRate));

    for (int c = 0; c < 2; ++c)
    {
        in[c].setSize (juce::jmax (phaseTaps, firDelay + 1) + 1);
        diff[c].setSize (taps + factor);
        pre[c].setSize (lookahead + 1);
        tubeDc[c].prepare ((float) osRate);
    }
    need.setSize (lookahead + 1);
    held.setSize (lookahead + 1);
    limit.setSize (lookahead + 1);

    // Tape emphasis: a first-order high shelf (bilinear) and its exact inverse.
    const float g = juce::Decibels::decibelsToGain (kTapeEmphasisDb);
    const float k = (float) std::tan (juce::MathConstants<double>::pi * kTapeEmphasisHz / osRate);
    for (int c = 0; c < 2; ++c)
    {
        tapePre[c].b0 = (g + k) / (1.0f + k);
        tapePre[c].b1 = (k - g) / (1.0f + k);
        tapePre[c].a1 = (k - 1.0f) / (1.0f + k);
        tapeDe[c].b0 = (1.0f + k) / (g + k);
        tapeDe[c].b1 = (k - 1.0f) / (g + k);
        tapeDe[c].a1 = (k - g) / (g + k);
    }

    drive.reset (osRate, kSmoothSeconds);
    ceiling.reset (osRate, kSmoothSeconds);
    reset();
}

void Clipper::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        in[c].clear();
        diff[c].clear();
        pre[c].clear();
        tubeDc[c].reset();
        tapePre[c].reset();
        tapeDe[c].reset();
    }
    need.clear();
    held.clear();
    limit.clear();
    for (int i = 0; i <= lookahead; ++i)
    {
        need.push (1.0f);
        held.push (1.0f);
        limit.push (std::numeric_limits<float>::max());
    }
    zeroRun = taps + factor;
    limGain = 1.0f;
    fading = false;
    fadePos = 0;
    first = true;
}

void Clipper::startMode (int m) noexcept
{
    const bool tubeWasOn = usesMode (tube), tapeWasOn = usesMode (tape);
    prevMode = mode;
    mode = m;
    fading = true;
    fadePos = 0;

    // A mode that wasn't running starts its filters from rest (it fades in from zero).
    for (int c = 0; c < 2; ++c)
    {
        if (m == tube && ! tubeWasOn)
            tubeDc[c].reset();
        if (m == tape && ! tapeWasOn)
        {
            tapePre[c].reset();
            tapeDe[c].reset();
        }
    }
}

/** shape (u) - u: what this mode adds to the (delayed) input. Exactly zero
    where the mode is a straight line and the drive is 0 dB. */
float Clipper::residual (int m, float u, float gain, float ceil, int ch) noexcept
{
    if (m == off)
        return 0.0f;

    const float v = u * (gain / ceil);
    switch (m)
    {
        case soft: return std::abs (v) <= kSoftKnee ? u * (gain - 1.0f) : ceil * softUnit (v) - u;
        case hard: return std::abs (v) <= 1.0f ? u * (gain - 1.0f) : ceil * hardUnit (v) - u;
        case fold: return std::abs (v) <= kFoldKnee ? u * (gain - 1.0f) : ceil * foldUnit (v) - u;
        case tube:
        {
            const auto t = tubeParts (v);
            return ceil * (t.odd + tubeDc[ch].process (t.even)) - u;
        }
        case tape:
        default:
            return ceil * tapeDe[ch].process (tapeUnit (tapePre[ch].process (v))) - u;
    }
}

void Clipper::process (float* left, float* right, int numSamples,
                       int newMode, float driveDb, float ceilingDb) noexcept
{
    newMode = juce::jlimit ((int) off, numModes - 1, newMode);
    drive.setTargetValue (juce::Decibels::decibelsToGain (sanitise (driveDb, 0.0f, 24.0f, 0.0f)));
    ceiling.setTargetValue (juce::Decibels::decibelsToGain (sanitise (ceilingDb, -12.0f, 0.0f, 0.0f)));

    if (first)
    {
        first = false;
        drive.setCurrentAndTargetValue (drive.getTargetValue());
        ceiling.setCurrentAndTargetValue (ceiling.getTargetValue());
        mode = prevMode = newMode;
        fading = false;
    }
    else if (! fading && newMode != mode)
    {
        startMode (newMode);
    }

    float* io[2] { left, right };
    const int usedWindow = taps + factor - 1; // the decimator reads the oldest `taps` of these

    for (int i = 0; i < numSamples; ++i)
    {
        in[0].push (io[0][i]);
        in[1].push (io[1][i]);

        // --- Shape at the oversampled rate, unless it's certain to change nothing
        bool shaping = mode != off || fading;
        if (shaping && ! fading && juce::exactlyEqual (drive.getTargetValue(), 1.0f) && ! drive.isSmoothing() && ! ceiling.isSmoothing())
        {
            if (const float knee = linearUpTo (mode); knee > 0.0f)
            {
                const auto r0 = juce::FloatVectorOperations::findMinAndMax (in[0].last (phaseTaps), phaseTaps);
                const auto r1 = juce::FloatVectorOperations::findMinAndMax (in[1].last (phaseTaps), phaseTaps);
                const float peak = juce::jmax (r0.getEnd(), -r0.getStart(), r1.getEnd(), -r1.getStart());
                shaping = peak * phaseGain > knee * ceiling.getCurrentValue();
            }
        }

        if (shaping)
        {
            const float* x0 = in[0].last (phaseTaps);
            const float* x1 = in[1].last (phaseTaps);
            for (int p = 0; p < factor; ++p)
            {
                const float g = drive.getNextValue();
                const float c = ceiling.getNextValue();
                const float* coefs = phases.data() + p * phaseTaps;
                const float u0 = dot (coefs, x0, phaseTaps);
                const float u1 = dot (coefs, x1, phaseTaps);
                float d0 = residual (mode, u0, g, c, 0);
                float d1 = residual (mode, u1, g, c, 1);
                if (fading)
                {
                    const float t = (float) fadePos / (float) fadeLength;
                    d0 = d0 * t + residual (prevMode, u0, g, c, 0) * (1.0f - t);
                    d1 = d1 * t + residual (prevMode, u1, g, c, 1) * (1.0f - t);
                    if (++fadePos >= fadeLength)
                        fading = false;
                }
                diff[0].push (d0);
                diff[1].push (d1);
                zeroRun = (juce::exactlyEqual (d0, 0.0f) && juce::exactlyEqual (d1, 0.0f)) ? juce::jmin (zeroRun + 1, usedWindow) : 0;
            }
        }
        else
        {
            drive.skip (factor);
            ceiling.skip (factor);
            if (zeroRun < usedWindow)
            {
                for (int p = 0; p < factor; ++p)
                {
                    diff[0].push (0.0f);
                    diff[1].push (0.0f);
                }
                zeroRun = juce::jmin (zeroRun + factor, usedWindow);
            }
        }

        // --- Decimate the difference and add it to the delayed input
        float y[2];
        for (int c = 0; c < 2; ++c)
        {
            y[c] = in[c].delayed (firDelay);
            if (zeroRun < usedWindow)
                y[c] += dot (fir.data(), diff[c].last (usedWindow), taps);
        }

        // --- Look-ahead limiter: holds the output at the ceiling where the
        //     band-limited signal overshoots. The gain is a moving average (over
        //     `lookahead` samples) of a running minimum (over the same span) of
        //     the gain each sample needs, so by the time a sample leaves the
        //     delay the gain is at or below what it needs. Release is a one-pole.
        //     While Off has a share w of a crossfade the limit is ceiling / (1 - w),
        //     so it lets the Off side through and closes in smoothly as the fade
        //     ends (switching it on at once would click). Off alone: no limit.
        float offShare = mode == off ? 1.0f : 0.0f;
        if (fading)
        {
            const float t = (float) fadePos / (float) fadeLength;
            offShare = mode == off ? t : prevMode == off ? 1.0f - t : 0.0f;
        }
        const float threshold = offShare >= 1.0f ? std::numeric_limits<float>::max()
                                                 : juce::jmin (std::numeric_limits<float>::max(),
                                                               ceiling.getCurrentValue() / (1.0f - offShare));
        float needed = 1.0f;
        if (const float peak = juce::jmax (std::abs (y[0]), std::abs (y[1])); peak > threshold)
            needed = threshold / peak;
        limit.push (threshold);
        need.push (needed);

        const float* nw = need.last (lookahead);
        float runMin = nw[0];
        for (int k = 1; k < lookahead; ++k)
            runMin = juce::jmin (runMin, nw[k]);
        held.push (runMin);

        const float* hw = held.last (lookahead);
        float sum = 0.0f;
        for (int k = 0; k < lookahead; ++k)
            sum += hw[k];
        const float average = sum / (float) lookahead;

        if (limGain < 1.0f)
        {
            limGain += (1.0f - limGain) * releaseCoef;
            if (limGain > 0.999999f)
                limGain = 1.0f;
        }
        limGain = juce::jmin (limGain, average);

        const float outLimit = limit.delayed (lookahead - 1); // the limit this sample was planned for
        for (int c = 0; c < 2; ++c)
        {
            pre[c].push (y[c]);
            io[c][i] = juce::jlimit (-outLimit, outLimit, pre[c].delayed (lookahead - 1) * limGain);
        }
    }
}

} // namespace drum
