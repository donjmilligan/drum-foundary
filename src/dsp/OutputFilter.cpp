// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "OutputFilter.h"

namespace drum
{

namespace
{
    constexpr double kPi = juce::MathConstants<double>::pi;
    constexpr double kGlideSeconds = 0.04;
    constexpr double kFadeSeconds = 0.005;

    /** Where the filter actually sits: far enough from DC and Nyquist that the
        prewarp stays well behaved. responseDb() applies the same limits. */
    inline double clampCutoff (double hz, double sampleRate) noexcept
    {
        return juce::jlimit (10.0, sampleRate * 0.49, hz);
    }

    inline float prewarp (double hz, double sampleRate) noexcept
    {
        return (float) std::tan (kPi * clampCutoff (hz, sampleRate) / sampleRate);
    }
}

//==============================================================================
int OutputFilter::slopeOrder (int slopeIndex) noexcept
{
    static constexpr int orders[] { 2, 4, 8 };
    return orders[juce::jlimit (0, 2, slopeIndex)];
}

float OutputFilter::responseDb (float freqHz, float highPassHz, int highPassSlope,
                                float lowPassHz, int lowPassSlope, double sampleRate) noexcept
{
    if (sampleRate <= 0.0)
        return 0.0f;

    // Each side is an analog Butterworth mapped by the bilinear transform with its
    // cutoff prewarped, so |H|^2 = 1 / (1 + W^2N) (low-pass) or W^2N / (1 + W^2N)
    // (high-pass), where W = tan (pi f / sr) / tan (pi fc / sr).
    const double f = juce::jlimit (0.0, sampleRate * 0.49999, (double) freqHz);
    const double tf = std::tan (kPi * f / sampleRate);
    double db = 0.0;

    if (! isHighPassOff (highPassHz))
    {
        const double w = std::pow (tf / prewarp (highPassHz, sampleRate), 2.0 * slopeOrder (highPassSlope));
        db += 10.0 * std::log10 (juce::jmax (1.0e-30, w / (1.0 + w)));
    }
    if (! isLowPassOff (lowPassHz))
    {
        const double w = std::pow (tf / prewarp (lowPassHz, sampleRate), 2.0 * slopeOrder (lowPassSlope));
        db -= 10.0 * std::log10 (1.0 + w);
    }
    return (float) juce::jmax (-300.0, db);
}

//==============================================================================
void OutputFilter::prepare (double sampleRate)
{
    lowCut.prepare (sampleRate);
    highCut.prepare (sampleRate);
}

void OutputFilter::reset() noexcept
{
    lowCut.reset();
    highCut.reset();
}

void OutputFilter::process (float* left, float* right, int numSamples, const DrumParams& p) noexcept
{
    lowCut.setTarget (p.outHP, p.outHPSlope);
    highCut.setTarget (p.outLP, p.outLPSlope);
    lowCut.process (left, right, numSamples);
    highCut.process (left, right, numSamples);
}

//==============================================================================
void OutputFilter::Cascade::setOrder (int newOrder) noexcept
{
    order = newOrder;
    // Butterworth pole pairs: Q_i = 1 / (2 cos ((2i + 1) pi / 2N)), and the SVF's
    // damping k is 1 / Q.
    for (int i = 0; i < numSections(); ++i)
        sections[(size_t) i].k = (float) (2.0 * std::cos ((2 * i + 1) * kPi / (2.0 * order)));
}

void OutputFilter::Cascade::setCutoff (float g) noexcept
{
    for (int i = 0; i < numSections(); ++i)
    {
        auto& s = sections[(size_t) i];
        s.a1 = 1.0f / (1.0f + g * (g + s.k));
        s.a2 = g * s.a1;
        s.a3 = g * s.a2;
    }
}

void OutputFilter::Cascade::reset() noexcept
{
    for (auto& s : sections)
        s.ic1[0] = s.ic1[1] = s.ic2[0] = s.ic2[1] = 0.0f;
}

//==============================================================================
void OutputFilter::Side::prepare (double sr)
{
    sampleRate = sr;
    cutoff.reset (sr, kGlideSeconds);
    cutoff.setCurrentAndTargetValue (highPass ? 20.0f : 20000.0f);
    fadeLength = updateInterval * juce::jmax (1, (int) std::lround (sr * kFadeSeconds / updateInterval));
    reset();
}

void OutputFilter::Side::setTarget (float cutoffHz, int slopeIndex) noexcept
{
    const float off = highPass ? 20.0f : 20000.0f;
    const float hz = std::isfinite (cutoffHz) ? juce::jlimit (1.0f, 100000.0f, cutoffHz) : off;
    targetOrder = slopeOrder (slopeIndex);

    if (! first)
    {
        cutoff.setTargetValue (hz);
        return;
    }

    // Start from the current settings rather than gliding to them.
    first = false;
    cutoff.setCurrentAndTargetValue (hz);
    const bool isOff = highPass ? isHighPassOff (hz) : isLowPassOff (hz);
    for (auto& c : cascades)
    {
        c.setOrder (0);
        c.reset();
    }
    current = 0;
    cascades[0].setOrder (isOff ? 0 : targetOrder);
    fadePos = -1;
    untilUpdate = 0;
    lastCutoff = -1.0f;
}

void OutputFilter::Side::update() noexcept
{
    const float fc = cutoff.skip (updateInterval);
    const bool isOff = highPass ? isHighPassOff (fc) : isLowPassOff (fc);
    const int wanted = isOff ? 0 : targetOrder;

    bool changed = ! juce::exactlyEqual (fc, lastCutoff);
    if (fadePos < 0 && wanted != cascades[current].order)
    {
        // Crossfade to a fresh cascade of the new order (or to the dry signal).
        auto& next = cascades[1 - current];
        next.setOrder (wanted);
        next.reset();
        fadePos = 0;
        changed = true;
    }

    if (changed)
    {
        const float g = prewarp (fc, sampleRate);
        cascades[0].setCutoff (g);
        cascades[1].setCutoff (g);
        lastCutoff = fc;
    }
}

void OutputFilter::Side::process (float* left, float* right, int numSamples) noexcept
{
    // Coefficient updates follow a sample count that carries across blocks, so the
    // result doesn't depend on how the host splits the audio.
    for (int i = 0; i < numSamples;)
    {
        if (untilUpdate == 0)
        {
            update();
            untilUpdate = updateInterval;
        }
        const int n = juce::jmin (numSamples - i, untilUpdate);
        run (left + i, right + i, n);
        i += n;
        untilUpdate -= n;
    }
}

void OutputFilter::Side::run (float* left, float* right, int numSamples) noexcept
{
    auto& cur = cascades[current];
    float* chans[2] { left, right };

    if (fadePos < 0)
    {
        if (cur.order > 0)
            for (int ch = 0; ch < 2; ++ch)
                runCascade (cur, chans[ch], ch, numSamples);
        return;
    }

    // numSamples never exceeds updateInterval here, and a fade always ends on an update.
    auto& next = cascades[1 - current];
    const float step = 1.0f / (float) fadeLength;
    for (int ch = 0; ch < 2; ++ch)
    {
        float* x = chans[ch];
        std::copy (x, x + numSamples, fadeScratch);
        if (cur.order > 0)
            runCascade (cur, x, ch, numSamples);
        if (next.order > 0)
            runCascade (next, fadeScratch, ch, numSamples);
        for (int i = 0; i < numSamples; ++i)
            x[i] += (fadeScratch[i] - x[i]) * ((float) (fadePos + i + 1) * step);
    }

    fadePos += numSamples;
    if (fadePos >= fadeLength)
    {
        current = 1 - current;
        fadePos = -1;
    }
}

void OutputFilter::Side::runCascade (Cascade& c, float* x, int ch, int numSamples) const noexcept
{
    for (int s = 0; s < c.numSections(); ++s)
    {
        auto& sec = c.sections[(size_t) s];
        const float k = sec.k, a1 = sec.a1, a2 = sec.a2, a3 = sec.a3;
        float ic1 = sec.ic1[ch], ic2 = sec.ic2[ch];

        for (int i = 0; i < numSamples; ++i)
        {
            const float v0 = x[i];
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            x[i] = highPass ? v0 - k * v1 - v2 : v2;
        }

        sec.ic1[ch] = ic1;
        sec.ic2[ch] = ic2;
    }
}

} // namespace drum
