// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include "CloudReverb.h"
#include "DspUtils.h"
#include <array>
#include <vector>

namespace drum
{

/** Stereo algorithmic reverb with a gate on its output, run as a send: it
    adds the wet signal to the buffer and leaves the dry signal alone.

    Two engines make the reverberation (Reverb Engine): Cloud, Ghost Note Audio's
    Cloud Seed (CloudReverb.h, the default), and Classic, the original FDN described
    below (patches from before Cloud load on it). Both share the send (Mix), the gate
    and the wet low cut and high cut; Cloud does its own pre-delay, and adds
    Diffusion, Mod Depth / Rate, Width, Early and Damping. Going idle stops the Cloud
    engine without clearing it (its tail is under -100 dB by then; clearing its
    ~20 MB on the audio thread isn't worth it).

    Classic:

    Signal path: send gain (Mix) -> pre-delay -> 6 series allpasses per
    channel (input diffusion) -> 8-line feedback delay network -> low cut ->
    high cut -> gate -> added to the dry signal.

    The FDN mixes its lines through an 8x8 Hadamard matrix, which is
    orthogonal, so the loop is lossless before the per-line gains. Each line's
    gain is 10^(-3 * delay / (RT60 * sr)), so every line loses 60 dB in Decay
    seconds whatever its length. Each line also has a one-pole damping filter
    that sets the RT60 at Nyquist to half of Decay, so highs die faster. Each
    line's length is slowly modulated by a fraction of a millisecond to break
    up metallic ringing. The left and right outputs are two different
    (orthogonal) sign patterns across the lines, so they are decorrelated.

    Size scales all line lengths (about 5 to 98 ms) and the last two input
    diffusers with them. A bigger Size puts the first reflection later, fills
    up more slowly (each reflection is smeared over a longer, denser cloud)
    and rings with more, closer-spaced modes (a smoother, less boxy tail), at
    the same Decay and about the same loudness. The loudness is kept by
    scaling the output by the lines' energy at 2 kHz (see controlTick): left
    alone, the short lines of a small room go round the loop so often that its
    tail is ~10-15 dB louder than a big one's, which made turning Size up
    sound like a smaller room. Decay stays an exact RT60 whatever the Size.

    The gate is a VCA on the wet output. Each hit opens it (1 ms attack); it
    stays open until Gate Time after the hit, then fades out over Gate Fade
    with a raised cosine and stays shut until the next hit. Gate Time counts
    from the hit itself (plus the master chain's latency, which is passed in
    as hitDelay), not from the pre-delayed start of the wet signal, so it is
    the point on the timeline where the tail is cut whatever the pre-delay.
    The reverb itself keeps running while the gate is shut, like a hardware
    gate on a reverb return: clearing it would need a large memset at every
    cut and would make a quick second hit sound different from the first
    only in the gated case.

    Mix scales what is sent in, not what comes out, so turning it down lets
    a ringing tail die away naturally. Once nothing is being sent and the
    tail has fallen below -100 dB, the reverb goes idle: its buffers are
    cleared and process() only keeps the gate's clock running, so Mix = 0
    costs almost nothing and leaves the dry signal bit-for-bit untouched.

    Real-time safe once prepare() has run: process() never allocates or locks. */
class Reverb
{
public:
    static constexpr int numLines = 8;
    static constexpr int numDiffusers = 6;
    static constexpr float maxPreDelayMs = 80.0f;
    static constexpr float gateAttackMs = 1.0f;

    /** Allocates every buffer for the largest Size and pre-delay at this rate. */
    void prepare (double sampleRate);
    /** Clears all state; the next process() starts from the current settings. */
    void reset() noexcept;

    /** Adds the wet signal into left/right in place. hitOffsets are the sample
        offsets (sorted, relative to this block) of the hits that start in it;
        each reaches this point hitDelay samples later. */
    void process (float* left, float* right, int numSamples, const DrumParams& p,
                  const int* hitOffsets, int numHits, int hitDelay) noexcept;

    bool isIdle() const noexcept { return idle; }

    /** How long after a hit the wet signal can still be heard, in seconds:
        0 when it's off or Mix is 0, Gate Time + Gate Fade with the gate on (or less, if the
        reverb dies out sooner), pre-delay + 1.5 x Decay with it off. */
    static double tailSeconds (const DrumParams& p) noexcept;

private:
    struct DelayLine
    {
        std::vector<float> buffer;
        int mask = 0, write = 0;

        void allocate (int minLength);
        void clear() noexcept;

        void push (float x) noexcept
        {
            buffer[(size_t) write] = x;
            write = (write + 1) & mask;
        }

        /** d samples before the most recent push (d >= 0), linearly interpolated. */
        float read (float d) const noexcept
        {
            const int di = (int) d;
            const float frac = d - (float) di;
            const float* b = buffer.data();
            const float a = b[(write - 1 - di) & mask];
            return a + frac * (b[(write - 2 - di) & mask] - a);
        }
    };

    struct Allpass
    {
        DelayLine line;
        float length = 1.0f; // samples (at full Size, for the ones that scale with it)
        float g = 0.6f;

        /** d: the delay in samples (>= 1), may change every sample. */
        float process (float x, float d) noexcept
        {
            const float z = line.read (d - 1.0f);
            const float w = x + g * z;
            line.push (w);
            return z - g * w;
        }
    };

    enum class GatePhase { closed, attack, open, fade };

    void updateTargets (const DrumParams& p) noexcept;
    void controlTick (bool snap) noexcept;
    float lfo (int line, int64_t atSample) const noexcept;
    float nextGateGain() noexcept;
    void skipBlock (int numSamples, const int* hitOffsets, int numHits, int hitDelay) noexcept;
    void clearBuffers() noexcept;
    void processCloud (float* left, float* right, int numSamples, const DrumParams& p,
                       const int* hitOffsets, int numHits, int hitDelay) noexcept;

    bool prepared = false;
    double sampleRate = 44100.0;

    // Buffers
    DelayLine preL, preR;
    std::array<DelayLine, numLines> lines;
    std::array<Allpass, numDiffusers> diffL, diffR;
    std::array<float, numLines> damp {};
    dsp::Svf lowCutL, lowCutR, highCutL, highCutR;

    // Fixed per sample rate
    std::array<float, numLines> lineBase {};  // samples at full Size
    std::array<double, numLines> lfoRate {};  // cycles per sample
    float modDepth = 1.0f, sendStep = 0.001f, maxPreDelay = 1.0f, tickSmooth = 0.1f;
    int idleAfter = 1;

    // Targets from the latest parameters
    float sendTarget = 0.0f, sizeTarget = 0.6f, rt60Target = 1.0f, preTarget = 0.0f;
    float lowCutTarget = 200.0f, highCutTarget = 8000.0f;
    bool gateOn = true;
    int gateSamples = 1, fadeSamples = 1;
    float attackStep = 0.02f;

    // Smoothed control values (updated every tick)
    float send = 0.0f, sizeSmoothed = 0.6f, logRt60 = 0.0f, preSmoothed = 0.0f;
    float logLowCut = 0.0f, logHighCut = 0.0f;
    std::array<float, numLines> lineCur {}, lineEnd {}, lineInc {}, lineGain {}, linePole {};
    float preCur = 0.0f, preEnd = 0.0f, preInc = 0.0f;
    float roomCur = 1.0f, roomEnd = 1.0f, roomInc = 0.0f;   // the room diffusers' length scale
    float outCur = 0.5f, outEnd = 0.5f, outInc = 0.0f;       // output gain (level vs Size)
    int samplesToTick = 0;
    int64_t clock = 0;
    bool resync = true, snapSend = true;

    // Idle detection
    bool idle = true;
    int quietSamples = 0;

    // The Cloud engine and its scratch (process() works through a block in chunks of this)
    static constexpr int cloudChunk = 256;
    CloudReverb cloud;
    std::array<float, cloudChunk> cloudInL {}, cloudInR {}, cloudOutL {}, cloudOutR {}, cloudGate {};
    int engine = revEngineCloud;
    bool cloudIdle = true;      // not feeding Cloud Seed (see processCloud())
    int64_t cloudQuiet = 0;     // samples of quiet in and out, while it runs

    // Gate
    GatePhase gatePhase = GatePhase::closed;
    int64_t sinceHit = 0;
    float gateGain = 0.0f, fadeFrom = 0.0f;
    int fadePos = 0;
};

} // namespace drum
