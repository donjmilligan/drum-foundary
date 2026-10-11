// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include "DrumEngine.h"
#include <vector>

namespace drum
{

/** Turns transients in a sidechain signal into hits for DrumEngine.

    Per sample it takes max(|L|, |R|) (non-finite samples count as silence) and
    follows it with a peak envelope: instant attack, a 15 ms hold, then a 10 ms
    exponential release. Without the hold, a sustained 50 Hz sine sags ~6 dB
    between its peaks (a 20 Hz one ~13 dB), enough to re-arm and fire every
    cycle; with it the sag stays under ~3.5 dB down to 20 Hz. The cost is
    15 ms more before the detector re-arms after a hit.

    A hit starts when the envelope reaches the threshold while armed. It then
    disarms, and re-arms only once the envelope has fallen 6 dB below the
    threshold, so a sustained signal fires once. A crossing that comes less
    than Retrigger ms after the previous hit is swallowed (and still disarms,
    so a flam doesn't fire late halfway through its own decay).

    Velocity comes from the peak over a short window after the crossing, so
    each hit is reported exactly velocityWindowMs after its crossing: a
    constant delay, which a DAW track delay can compensate, rather than jitter.
    The peak maps from the threshold (0.3) to 0 dBFS (1.0), linear in dB.

    All state carries over between blocks, so a hit whose window straddles a
    block boundary is reported in the next block. Real-time safe once
    prepare() has run: process() never allocates or locks. */
class TransientTrigger
{
public:
    static constexpr double velocityWindowMs = 1.5;
    static constexpr double holdMs = 15.0;
    static constexpr double releaseMs = 10.0;
    static constexpr float rearmDb = 6.0f;
    static constexpr float minVelocity = 0.3f;

    /** Sizes the hit list for blocks of up to maxBlockSize samples at the
        shortest Retrigger time. Allocates: call from prepareToPlay. */
    void prepare (double sampleRate, int maxBlockSize);
    /** Forgets any envelope, pending hit and retrigger lockout. */
    void reset() noexcept;

    /** Scans numSamples of `sidechain` (any channel count; 0 channels means no
        signal) and replaces the hit list with this block's hits, as sorted
        sample offsets in [0, numSamples). With the trigger off it resets and
        reports nothing. */
    void process (const juce::AudioBuffer<float>& sidechain, int numSamples, const SidechainParams& params) noexcept;

    const TriggerHit* getHits() const noexcept { return hits.data(); }
    int getNumHits() const noexcept { return numHits; }

    /** Delay between a crossing and its reported hit, in samples. */
    int getReportDelaySamples() const noexcept { return windowSamples; }

private:
    void addHit (int offset, float peak) noexcept;

    double sampleRate = 44100.0;
    int windowSamples = 66;
    int holdSamples = 662;
    float releaseCoef = 0.99f;

    // Detector state (carries across blocks)
    float envelope = 0.0f;
    int holdLeft = 0;
    bool armed = true;
    int64_t samplesSinceHit = 0;
    int captureLeft = 0; // > 0 while measuring a hit's peak
    float capturePeak = 0.0f;
    float thresholdGain = 0.0f;

    std::vector<TriggerHit> hits;
    int numHits = 0;
};

} // namespace drum
