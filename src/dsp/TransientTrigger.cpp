// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "TransientTrigger.h"
#include <cmath>

namespace drum
{

namespace
{
    /** The Retrigger parameter's lower bound: the densest hits can come. */
    constexpr double minRetriggerMs = 10.0;
}

void TransientTrigger::prepare (double sr, int maxBlockSize)
{
    sampleRate = (std::isfinite (sr) && sr > 0.0) ? sr : 44100.0;

    windowSamples = juce::jmax (1, juce::roundToInt (velocityWindowMs * 0.001 * sampleRate));
    holdSamples   = juce::jmax (0, juce::roundToInt (holdMs * 0.001 * sampleRate));
    releaseCoef   = (float) std::exp (-1.0 / (releaseMs * 0.001 * sampleRate));

    // One hit per Retrigger period at most, plus one that was pending from the
    // previous block. Sized for blocks well beyond the promised maximum, since
    // some hosts overshoot; past that, extra hits in a block are dropped.
    const int blockForSizing = juce::jmax (maxBlockSize, 16384);
    const int minRetriggerSamples = juce::jmax (1, (int) (minRetriggerMs * 0.001 * sampleRate));
    hits.assign ((size_t) (blockForSizing / minRetriggerSamples + 2), TriggerHit {});

    reset();
}

void TransientTrigger::reset() noexcept
{
    envelope = 0.0f;
    holdLeft = 0;
    armed = true;
    samplesSinceHit = std::numeric_limits<int64_t>::max() / 2;
    captureLeft = 0;
    capturePeak = 0.0f;
    numHits = 0;
}

void TransientTrigger::addHit (int offset, float peak) noexcept
{
    if (numHits >= (int) hits.size())
        return;

    // Peak from the threshold up to 0 dBFS -> minVelocity..1, linear in dB.
    const float thresholdDb = juce::Decibels::gainToDecibels (thresholdGain, -100.0f);
    const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
    const float t = thresholdDb > -0.01f ? 1.0f
                                         : juce::jlimit (0.0f, 1.0f, (peakDb - thresholdDb) / -thresholdDb);

    hits[(size_t) numHits++] = { offset, minVelocity + (1.0f - minVelocity) * t };
}

void TransientTrigger::process (const juce::AudioBuffer<float>& sidechain, int numSamples,
                                const SidechainParams& params) noexcept
{
    numHits = 0;
    numSamples = juce::jmin (numSamples, sidechain.getNumSamples());
    const int numChannels = sidechain.getNumChannels();

    if (! params.enabled || numChannels <= 0 || numSamples <= 0 || hits.empty())
    {
        reset();
        return;
    }

    const float thresholdDb = std::isfinite (params.thresholdDb) ? juce::jlimit (-100.0f, 0.0f, params.thresholdDb) : -24.0f;
    const float retriggerMs = std::isfinite (params.retriggerMs) ? juce::jmax ((float) minRetriggerMs, params.retriggerMs) : 50.0f;

    thresholdGain = juce::Decibels::decibelsToGain (thresholdDb);
    const float rearmGain = juce::Decibels::decibelsToGain (thresholdDb - rearmDb);
    // Never shorter than the velocity window, so two captures can't overlap.
    const auto retriggerSamples = (int64_t) juce::jmax ((double) windowSamples + 1.0, retriggerMs * 0.001 * sampleRate);

    const float* left  = sidechain.getReadPointer (0);
    const float* right = numChannels > 1 ? sidechain.getReadPointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        // Non-finite input reads as silence (the comparison is false for NaN).
        float x = std::abs (left[i]);
        if (! (x <= std::numeric_limits<float>::max()))
            x = 0.0f;
        if (right != nullptr)
        {
            const float r = std::abs (right[i]);
            if (r <= std::numeric_limits<float>::max() && r > x)
                x = r;
        }

        // Peak envelope: instant attack, hold, exponential release.
        if (x >= envelope)
        {
            envelope = x;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            envelope *= releaseCoef;
            if (envelope < 1.0e-9f)
                envelope = 0.0f;
        }

        ++samplesSinceHit;

        if (captureLeft > 0)
        {
            capturePeak = juce::jmax (capturePeak, x);
            if (--captureLeft == 0)
                addHit (i, capturePeak);
        }
        else if (armed)
        {
            if (envelope >= thresholdGain)
            {
                armed = false;
                if (samplesSinceHit >= retriggerSamples)
                {
                    samplesSinceHit = 0;
                    captureLeft = windowSamples;
                    capturePeak = x;
                }
            }
        }
        else if (envelope < rearmGain)
        {
            armed = true;
        }
    }
}

} // namespace drum
