// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include "Clipper.h"
#include "Compressor.h"
#include "DriveStage.h"
#include "DspUtils.h"
#include "OutputFilter.h"
#include "ParametricEq.h"
#include "Reverb.h"
#include <juce_dsp/juce_dsp.h>

namespace drum
{

/** The master chain: the modules of the patch's FxChain in its order (two Drives,
    the EQ, two compressors, two clippers and the reverb, each at most once; by
    default Drive -> EQ -> Reverb), then the fixed stages: output low cut / high
    cut -> output gain -> output stage (Off, a Clipper shape, or Comp: the output
    compressor; see clipModeNames).

      Drive    DriveStage: Soft / Hard / Tube / Fold / Crush at 2x, then a DC blocker.
      EQ       ParametricEq.
      Comp     Compressor (feed-forward, no latency).
      Clip     a Clipper at one of its shapes (Off = only its delay).
      Reverb   a parallel send, added to the dry; its gate opens on each hit as the
               hit reaches the reverb (after whatever sits before it in the chain).

    Latency never depends on the chain: every module that has latency (the Drives'
    oversamplers, the Clippers' FIRs) always runs its delay while it is in the chain,
    and whatever modules aren't in it are made up for by a delay at the very end,
    after the output stage. So getLatencySamples() is two Drives + three Clippers,
    whatever the order, and the chain before that delay runs with the same timing
    as it always did.

    Changing the order fades the chain's output out (10 ms), holds it silent until what
    was still on its way through the output stage and the end delay has come out
    (under 3 ms), switches (every module starting from a reset), and fades back in. Each module's on switch fades
    as it always has (a Drive's Mix to 0, the EQ's bands out, a compressor's mix to
    dry, a clipper to its Off, the reverb's wet signal out). */
class MasterChain
{
public:
    MasterChain() = default;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Processes numSamples of a stereo buffer in place. numSamples must not
        exceed the prepared block size. hitOffsets (sorted, relative to this
        block) are where hits start in the voices' signal; the reverb gate
        opens on each one once it has come through what's ahead of the reverb. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const DrumParams& p,
                  const int* hitOffsets = nullptr, int numHits = 0) noexcept;

    /** The whole chain's latency, the same for any FxChain (see the class notes). */
    int getLatencySamples() const noexcept;
    /** How far the pre-clip capture runs ahead of the output: the output stage's
        Clipper plus the delay at the end that stands in for modules not in the chain. */
    int getPreClipLeadSamples() const noexcept { return clipper.getLatencySamples() + endDelaySamples; }
    /** The output stage Clipper's latency. */
    int getClipperLatencySamples() const noexcept { return clipper.getLatencySamples(); }

    /** Offline renders only (not for the audio thread): while set, every block's
        output stage input times its gain if nothing were taken off (a Clipper's Drive,
        the output compressor's Makeup; Off has none) is appended to `buffer` (stereo)
        until it is full. Ahead of the output by getPreClipLeadSamples(). nullptr stops it. */
    void setPreClipCapture (juce::AudioBuffer<float>* buffer) noexcept
    {
        preClip = buffer;
        preClipPos = 0;
    }

    /** Offline renders only: captures chain compressor `n` (0, 1) or the output compressor
        (n < 0): see Compressor::setCapture(). nullptr stops it. */
    void setCompCapture (int n, juce::AudioBuffer<float>* buffer) noexcept
    {
        (n < 0 ? outComp : comps[(size_t) juce::jlimit (0, 1, n)]).setCapture (buffer);
    }
    /** How far compressor `n`'s capture runs ahead of the output: the latency of what comes
        after it in the chain now running, the output stage and the end delay. -1 when a
        chain compressor isn't in the chain. */
    int getCompLeadSamples (int n) const noexcept;

    /** The gain EQ band `band` applies now (a dynamic band moves), for the UI. */
    float getEqAppliedGainDb (int band) const noexcept { return eq.getAppliedGainDb (band); }

    /** Gain reduction (dB, <= 0) in the last block of chain compressor `n` (0, 1), or of
        the output compressor (n < 0), for meters. */
    float getCompReductionDb (int n) const noexcept
    {
        return n < 0 ? outComp.getReductionDb() : comps[(size_t) juce::jlimit (0, 1, n)].getReductionDb();
    }

    /** For tests: with the reverb disabled the chain runs exactly as it did
        before the reverb existed. */
    void setReverbEnabled (bool shouldBeEnabled) noexcept { reverbEnabled = shouldBeEnabled; }
    bool isReverbIdle() const noexcept { return reverb.isIdle(); }

    /** For tests: with the output filter disabled the chain runs exactly as it
        did before the filter existed (with both sides off it should anyway). */
    void setOutputFilterEnabled (bool shouldBeEnabled) noexcept { outputFilterEnabled = shouldBeEnabled; }

    /** The chain that is running now (it lags a changed FxChain by its crossfade). */
    const FxChain& getActiveChain() const noexcept { return active; }

private:
    int moduleLatency (int module) const noexcept;
    int chainLatency (const FxChain& c) const noexcept;
    int maxChainLatency() const noexcept;
    void switchTo (const FxChain& next) noexcept;
    void runModule (int module, float* L, float* R, int n, const DrumParams& p,
                    const int* hitOffsets, int numHits, int latencyBefore) noexcept;
    void runReverb (float* L, float* R, int n, const DrumParams& p, const int* hitOffsets, int numHits,
                    int latencyBefore) noexcept;

    float sampleRate = 44100.0f;

    DriveStage drives[2];
    ParametricEq eq;
    Compressor comps[2];
    Clipper chainClippers[2];

    juce::SmoothedValue<float> outGain;
    juce::SmoothedValue<float> revLevel;         // the wet level; follows the reverb switch
    std::vector<float> dryL, dryR;               // the reverb's input while revLevel moves
    bool reverbCleared = false;                  // switched off, faded out and reset
    Reverb reverb;
    bool reverbEnabled = true;
    OutputFilter outputFilter;
    bool outputFilterEnabled = true;
    Compressor outComp;
    Clipper clipper;
    juce::AudioBuffer<float>* preClip = nullptr; // see setPreClipCapture()
    int preClipPos = 0;

    // The chain as it runs, and the crossfade to a changed one.
    FxChain active = FxChain::defaults();
    enum class Fade { none, out, hold, in };
    Fade fade = Fade::none;
    float fadeGain = 1.0f, fadeStep = 0.0f;
    int holdLeft = 0;
    FxChain pending;

    // The delay at the end that stands in for the modules that aren't in the chain.
    std::vector<float> endDelay[2];
    int endDelaySize = 1, endDelayPos = 0, endDelaySamples = 0;

    bool firstBlock = true;
};

} // namespace drum
