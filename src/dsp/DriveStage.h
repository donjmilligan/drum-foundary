// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "DspUtils.h"
#include "FxChain.h"
#include <juce_dsp/juce_dsp.h>

namespace drum
{

/** One Drive module of the master chain: Soft / Hard / Tube / Fold / Crush at 2x
    (half-band polyphase IIR, integer latency), then a 10 Hz DC blocker. Off takes
    its Mix to 0 over 20 ms; the oversampler always runs, so the latency never
    changes. This is the master chain's original drive, moved here unchanged so
    the chain can hold two. prepare() allocates; process() doesn't. */
class DriveStage
{
public:
    DriveStage() : oversampler (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true) {}

    void prepare (double sr, int maxBlockSize)
    {
        oversampler.initProcessing ((size_t) maxBlockSize);
        const double osRate = sr * (double) oversampler.getOversamplingFactor();
        preGain.reset (osRate, 0.02);
        makeup.reset (osRate, 0.02);
        mix.reset (osRate, 0.02);
        for (auto& d : dc)
            d.prepare ((float) sr);
        reset();
    }

    void reset() noexcept
    {
        oversampler.reset();
        for (auto& d : dc)
            d.reset();
        crushHold = 0;
        crushHeld[0] = crushHeld[1] = 0.0f;
        first = true;
    }

    int getLatencySamples() const noexcept { return (int) std::lround (oversampler.getLatencyInSamples()); }

    void process (float* left, float* right, int numSamples, const DriveSettings& s) noexcept
    {
        const bool crush = s.type == 4;
        preGain.setTargetValue (crush ? 1.0f : juce::Decibels::decibelsToGain (s.amount * 30.0f));
        makeup.setTargetValue (crush ? 1.0f : juce::Decibels::decibelsToGain (s.amount * -12.0f));
        mix.setTargetValue (s.on ? s.mix : 0.0f);
        if (first)
        {
            // Start from the current settings rather than ramping up from zero.
            first = false;
            for (auto* v : { &preGain, &makeup, &mix })
                v->setCurrentAndTargetValue (v->getTargetValue());
        }

        float* chans[2] { left, right };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) numSamples);
        auto up = oversampler.processSamplesUp (block);
        float* ch[2] { up.getChannelPointer (0), up.getChannelPointer (1) };
        const int upN = (int) up.getNumSamples();

        auto driveLoop = [&] (auto&& shaper)
        {
            for (int i = 0; i < upN; ++i)
            {
                const float pg = preGain.getNextValue();
                const float mk = makeup.getNextValue();
                const float mx = mix.getNextValue();
                for (int c = 0; c < 2; ++c)
                {
                    const float x = ch[c][i];
                    const float y = shaper (x * pg, c) * mk;
                    ch[c][i] = x + (y - x) * mx;
                }
            }
        };

        switch (s.type)
        {
            case 0: driveLoop ([] (float x, int) { return std::tanh (x); }); break;
            case 1: driveLoop ([] (float x, int) { return juce::jlimit (-1.0f, 1.0f, x); }); break;
            case 2: driveLoop ([] (float x, int) { return std::tanh (x + 0.25f) - 0.24491866f; }); break;
            case 3: driveLoop ([] (float x, int) { return dsp::fastSin01 (x * 0.25f); }); break;
            case 4:
            default:
            {
                // Bit depth 16 -> 3 bits, plus sample-and-hold decimation.
                const float bits = 16.0f - s.amount * 13.0f;
                const float levels = std::exp2 (bits - 1.0f);
                const int holdLen = 1 + (int) (s.amount * s.amount * 24.0f);
                for (int i = 0; i < upN; ++i)
                {
                    const float mx = mix.getNextValue();
                    preGain.skip (1);
                    makeup.skip (1);
                    const bool take = ++crushHold >= holdLen;
                    if (take)
                        crushHold = 0;
                    for (int c = 0; c < 2; ++c)
                    {
                        const float x = ch[c][i];
                        if (take)
                            crushHeld[c] = std::round (x * levels) / levels;
                        ch[c][i] = x + (crushHeld[c] - x) * mx;
                    }
                }
                break;
            }
        }

        oversampler.processSamplesDown (block);

        for (int i = 0; i < numSamples; ++i)
        {
            left[i] = dc[0].process (left[i]);
            right[i] = dc[1].process (right[i]);
        }
    }

private:
    juce::dsp::Oversampling<float> oversampler;
    juce::SmoothedValue<float> preGain, makeup, mix;
    dsp::DcBlocker dc[2];
    int crushHold = 0;
    float crushHeld[2] {};
    bool first = true;
};

} // namespace drum
