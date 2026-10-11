// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "OfflineRenderer.h"
#include "Reverb.h"

namespace drum
{

juce::AudioBuffer<float> renderHit (const DrumParams& params, const EnvTables& tables,
                                    double sampleRate, int midiNote, float velocity, uint32_t seed,
                                    const SampleSet& samples, juce::AudioBuffer<float>* preClip, CompCaptures* comps)
{
    constexpr int blockSize = 512;
    constexpr double tailSeconds = 0.03;    // lets drive/EQ ring out past Length
    constexpr double maxReverbSeconds = 8.0;
    constexpr float silenceDb = -90.0f;     // a reverb tail ends here, relative to the peak
    constexpr double silenceHoldSeconds = 0.05;

    juce::ScopedNoDenormals noDenormals;

    if (! std::isfinite (sampleRate) || sampleRate <= 0.0 || ! std::isfinite (params.lengthMs))
        return {};

    auto engine = std::make_unique<DrumEngine>();
    engine->prepare (sampleRate, blockSize);
    engine->setParams (params);
    engine->setTables (tables);
    engine->setSamples (samples);
    engine->setSeed (seed);

    // The dry hit is Length (plus the latest layer's Offset) plus a little; a
    // reverb tail can run past that, up to its own estimate (gate time + fade,
    // counted from the hit, or 1.5 x RT60 after the latest layer starts without
    // the gate), but the render stops early once the output has stayed 90 dB
    // under its peak for a while. With Mix at 0 this is exactly the dry length.
    const int latency = engine->getLatencySamples();
    // (The tiny subtraction keeps rounding noise from adding a sample.)
    const int dryLength = (int) std::ceil ((DrumEngine::hitLengthSeconds (params) + tailSeconds) * sampleRate - 1.0e-6);
    const double lateSeconds = params.revGate ? 0.0 : DrumVoice::latestOffsetMs (params) * 0.001;
    const double reverbTail = juce::jmin (maxReverbSeconds, Reverb::tailSeconds (params) + lateSeconds);
    const int length = juce::jmax (dryLength, (int) std::ceil ((reverbTail + 0.005) * sampleRate));
    const int total = latency + length;
    const int silenceHold = (int) (silenceHoldSeconds * sampleRate);

    juce::AudioBuffer<float> raw (2, total);
    juce::AudioBuffer<float> rawPre;
    if (preClip != nullptr)
    {
        rawPre.setSize (2, total);
        rawPre.clear();
        engine->setPreClipCapture (&rawPre);
    }
    // Each compressor that's working, captured from the start (aligned at the end).
    std::array<juce::AudioBuffer<float>, 3> rawComp;
    const auto compWorks = [&params] (int k)
    {
        return k < 2 ? params.comp[(size_t) k].on && params.chain.contains (k == 0 ? fxComp1 : fxComp2)
                     : params.clipMode == clipModeComp;
    };
    if (comps != nullptr)
        for (int k = 0; k < 3; ++k)
            if (compWorks (k))
            {
                rawComp[(size_t) k].setSize (3, total);
                rawComp[(size_t) k].clear();
                engine->setCompCapture (k < 2 ? k : -1, &rawComp[(size_t) k]);
            }

    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;

    float peak = 0.0f;
    int lastLoud = 0; // in raw's samples
    int rendered = 0;

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        block.setSize (2, n, false, false, true);

        midi.clear();
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, velocity), 0);

        engine->process (block, midi);
        raw.copyFrom (0, pos, block, 0, 0, n);
        raw.copyFrom (1, pos, block, 1, 0, n);
        rendered = pos + n;

        if (length > dryLength)
        {
            peak = juce::jmax (peak, block.getMagnitude (0, n));
            const float threshold = peak * juce::Decibels::decibelsToGain (silenceDb);
            for (int c = 0; c < 2; ++c)
            {
                const float* x = block.getReadPointer (c);
                for (int i = n; --i >= 0;)
                    if (std::abs (x[i]) > threshold)
                    {
                        lastLoud = juce::jmax (lastLoud, pos + i);
                        break;
                    }
            }

            if (rendered >= latency + dryLength && rendered - lastLoud > silenceHold)
                break;
        }
    }

    const int resultLength = juce::jmin (length, juce::jmax (dryLength, lastLoud + 1 - latency));
    juce::AudioBuffer<float> result (2, resultLength);
    result.copyFrom (0, 0, raw, 0, latency, resultLength);
    result.copyFrom (1, 0, raw, 1, latency, resultLength);

    if (preClip != nullptr)
    {
        // The output stage's input runs ahead of the output by its Clipper and the delay
        // at the end of the chain.
        engine->setPreClipCapture (nullptr);
        const int from = latency - engine->getPreClipLeadSamples();
        preClip->setSize (2, resultLength);
        preClip->clear();
        const int n = juce::jmin (resultLength, total - from);
        for (int c = 0; c < 2; ++c)
            preClip->copyFrom (c, 0, rawPre, c, from, juce::jmax (0, n));
    }
    if (comps != nullptr)
    {
        // A compressor's capture runs ahead of the output by what comes after it.
        for (int k = 0; k < 3; ++k)
        {
            auto& out = comps->comp[(size_t) k];
            engine->setCompCapture (k < 2 ? k : -1, nullptr);
            const int lead = engine->getCompLeadSamples (k < 2 ? k : -1);
            if (rawComp[(size_t) k].getNumSamples() == 0 || lead < 0)
            {
                out.setSize (3, 0);
                continue;
            }
            const int from = latency - lead;
            out.setSize (3, resultLength);
            out.clear();
            const int n = juce::jmin (resultLength, total - from);
            for (int c = 0; c < 3; ++c)
                if (from >= 0 && n > 0)
                    out.copyFrom (c, 0, rawComp[(size_t) k], c, from, n);
        }
    }
    return result;
}

} // namespace drum
