// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "DrumEngine.h"

namespace drum
{

/** What each compressor did in a render (renderHit()'s `comps`). */
struct CompCaptures
{
    /** [0] Comp 1, [1] Comp 2, [2] the output compressor. Each is 3 channels, aligned with
        the result: the compressor's input (mono), its output (mono, after Mix) and its gain
        change in dB (<= 0). Empty (0 samples) for a compressor that isn't in the chain or
        is off (the output one: unless Clip Mode is Comp). */
    std::array<juce::AudioBuffer<float>, 3> comp;
};

/** Renders one complete hit with the real engine, latency-compensated.
    Deterministic for a given seed, so the waveform display doesn't flicker.
    The result is Length + 30 ms long (plus the largest positive layer Offset,
    see DrumEngine::hitLengthSeconds()), or longer when the reverb is on: then it
    runs until the tail ends (the gate has shut, or it is 90 dB under the
    peak), capped at 8 s.

    With `preClip`, it also gets the signal going into the output clipper times the
    clipper's Drive (what would come out if nothing clipped), aligned sample for
    sample with the result: the difference is what the clipper took off.

    With `comps`, it also gets what each compressor did (see CompCaptures). */
juce::AudioBuffer<float> renderHit (const DrumParams& params,
                                    const EnvTables& tables,
                                    double sampleRate,
                                    int midiNote = 60,
                                    float velocity = 1.0f,
                                    uint32_t seed = 0x5EEDu,
                                    const SampleSet& samples = {},
                                    juce::AudioBuffer<float>* preClip = nullptr,
                                    CompCaptures* comps = nullptr);

} // namespace drum
