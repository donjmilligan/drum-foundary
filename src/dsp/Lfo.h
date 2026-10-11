// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include "DspUtils.h"
#include <array>

namespace drum
{

/** The three LFOs. They're global, not per voice: Retrig and One Shot restart
    every LFO at its Phase on each hit (a newer hit restarts them under an older
    one that's still ringing), Free runs on regardless. One Shot stops at the end
    of its first cycle and holds the value it reached.

    Outputs are bipolar, -1..1. The engine reads them once per control block (see
    DrumEngine) and moves the routed parameters with applyModulation(). S&H picks
    a new random value every cycle; Drift glides between them. Their random
    sequence restarts from the hit's seed, so a render is repeatable. */
class LfoBank
{
public:
    /** The shape's value at `t` cycles into it (wrapped to 0..1). `held` / `previous`
        are S&H's and Drift's random values: the cycle's own, and the one before. */
    static float shapeValue (int shape, float t, float held = 0.0f, float previous = 0.0f) noexcept
    {
        const float p = dsp::wrap01 (t);
        switch (shape)
        {
            case 0: return dsp::fastSin01 (p);                       // Sine
            case 1: return dsp::triangle01 (p);                      // Triangle
            case 2: return 1.0f - 2.0f * p;                          // Saw Down
            case 3: return 2.0f * p - 1.0f;                          // Saw Up
            case 4: return p < 0.5f ? 1.0f : -1.0f;                  // Square
            case 5: return held;                                     // S&H
            case 6: { const float s = p * p * (3.0f - 2.0f * p);     // Drift
                      return previous + (held - previous) * s; }
            default: return 0.0f;
        }
    }

    /** Cycles per second for an LFO's settings at `bpm`. */
    static double cyclesPerSecond (const DrumParams::Lfo& l, float bpm) noexcept
    {
        if (const float beats = lfoSyncBeats (l.sync); beats > 0.0f)
            return (double) juce::jlimit (20.0f, 999.0f, std::isfinite (bpm) ? bpm : 120.0f) / 60.0 / (double) beats;
        return std::isfinite (l.rateHz) ? juce::jlimit (0.0, 200.0, (double) l.rateHz) : 1.0;
    }

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        for (auto& s : states)
            s = {};
    }

    /** A hit: Retrig and One Shot LFOs start over at their Phase. */
    void retrigger (const DrumParams& p, uint32_t seed) noexcept
    {
        for (size_t i = 0; i < states.size(); ++i)
        {
            if (p.lfos[i].mode == 2) // Free
                continue;

            auto& s = states[i];
            s.cycles = 0.0;
            s.done = false;
            s.rng = dsp::mixSeed (seed, 0x51ED27u + (uint32_t) i * 0x9E3779B9u);
            s.previous = nextRandom (s.rng);
            s.held = nextRandom (s.rng);
        }
    }

    /** Moves every LFO on by `numSamples`. */
    void advance (const DrumParams& p, int numSamples) noexcept
    {
        if (numSamples <= 0)
            return;

        for (size_t i = 0; i < states.size(); ++i)
        {
            auto& s = states[i];
            const auto& l = p.lfos[i];
            if (s.done)
                continue;

            const double before = s.cycles + (double) l.phase;
            s.cycles += cyclesPerSecond (l, p.bpm) * (double) numSamples / sampleRate;
            if (l.mode == 1 && s.cycles >= 1.0)
            {
                s.cycles = 1.0;
                s.done = true;
            }

            // A new random value for every cycle boundary crossed (only the last one matters).
            if (std::floor (s.cycles + (double) l.phase) > std::floor (before))
            {
                s.previous = s.held;
                s.held = nextRandom (s.rng);
            }

            if (l.mode == 2 && s.cycles > 1.0e6) // keep a free-running phase precise
                s.cycles -= std::floor (s.cycles);
        }
    }

    /** The current outputs, -1..1. */
    void getValues (const DrumParams& p, float* out) const noexcept
    {
        for (size_t i = 0; i < states.size(); ++i)
        {
            const auto& s = states[i];
            const auto& l = p.lfos[i];
            // A finished One Shot holds the value just before its cycle wrapped.
            const double t = s.done ? (double) l.phase + 0.99999 : s.cycles + (double) l.phase;
            out[i] = shapeValue (l.shape, (float) (t - std::floor (t)), s.held, s.previous);
        }
    }

private:
    struct State
    {
        double cycles = 0.0;     // since the last restart
        bool done = false;       // a One Shot that has finished its cycle
        uint32_t rng = 0x1234567u;
        float held = 0.0f, previous = 0.0f;
    };

    static float nextRandom (uint32_t& state) noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    double sampleRate = 44100.0;
    std::array<State, kNumLfos> states {};
};

} // namespace drum
