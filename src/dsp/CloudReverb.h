// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <array>
#include <memory>

namespace Cloudseed { class ReverbController; }

namespace drum
{

/** The Cloud reverb engine: Ghost Note Audio's Cloud Seed algorithm (MIT; vendored in
    libs/CloudSeedCore, see its PATCHES.md), wet only. Stereo in, stereo out: a
    pre-delay, an early allpass diffuser, then eight modulated delay lines, each with
    its own diffuser and a high shelf (Damping) in its feedback, summed to each side
    with different random seeds (Width).

    The Reverb class runs it as a send and keeps its gate, Mix, low cut and high cut:
    this is only the reverberator. It's deterministic: the same settings always give
    the same tail (the vendored code's modulation phases are seeded, not random).

    Cloud Seed's feedback loops include its block size (a line's feedback is the previous
    block's output), so it always runs in fixed chunks of `chunk` samples through a
    small FIFO, whatever the host's blocks: the same tail at any block size, for a wet
    delay of `chunk` samples (0.7 ms at 48 kHz).

    prepare() allocates (about 20 MB at any sample rate up to 192 kHz); process()
    doesn't, and only passes on settings that changed. */
class CloudReverb
{
public:
    struct Settings
    {
        float size = 0.6f;          // 0..1: delay line length, ~15 to 300 ms (less at very high rates)
        float decaySeconds = 2.5f;  // RT60 of the late lines
        float preDelayMs = 10.0f;
        float diffusion = 0.7f;     // 0..1: allpass feedback and stage count
        float modDepth = 0.3f;      // 0..1: delay modulation, lines and diffusers
        float modRate = 0.3f;       // 0..1: ~0.1 to 2.5 Hz
        float width = 1.0f;         // 0 = mono (same seeds both sides) .. 1 = fully decorrelated
        float early = 0.2f;         // 0..1: level of the early (diffused) part next to the late tail
        float damping = 0.4f;       // 0..1: high shelf in the lines, 0 to -18 dB (highs die faster)
    };

    CloudReverb();
    ~CloudReverb();

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Wet only, into outL / outR (which may not alias the inputs). */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const Settings& s) noexcept;

    /** The settings the next chunks use (they're applied at chunk starts). */
    void setSettings (const Settings& s) noexcept { settings = s; }

    /** One sample in, one out (`chunk` samples late). */
    void processSample (float inL, float inR, float& outL, float& outR) noexcept
    {
        fifoInL[(size_t) fifoPos] = inL;
        fifoInR[(size_t) fifoPos] = inR;
        outL = fifoOutL[(size_t) fifoPos];
        outR = fifoOutR[(size_t) fifoPos];
        if (++fifoPos == chunk)
            runChunk();
    }

    /** Picks up after a pause (samples not fed in): the FIFO restarts empty at
        `position` within a chunk, so chunks stay where an outside clock puts them.
        Cloud Seed's own state is left as it was. */
    void resume (int position) noexcept;

    /** The longest line size the buffers allow at this rate, ms. */
    float maxLineMs() const noexcept { return lineMsLimit; }

    static constexpr int chunk = 32;

private:
    void apply (const Settings& s, bool force) noexcept;
    void runChunk() noexcept;
    Settings settings;
    void set (int id, double value, bool force) noexcept;

    std::unique_ptr<Cloudseed::ReverbController> controller;
    std::array<double, 64> applied {};   // what each Cloud Seed parameter was last set to
    double sampleRate = 48000.0;
    float lineMsLimit = 300.0f, allpassMsLimit = 100.0f;
    bool fresh = true;

    // The fixed-chunk FIFO: what's come in since the last chunk, and what the last chunk made.
    std::array<float, chunk> fifoInL {}, fifoInR {}, fifoOutL {}, fifoOutR {};
    int fifoPos = 0;
};

} // namespace drum
