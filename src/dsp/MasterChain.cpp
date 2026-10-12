// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "MasterChain.h"

namespace drum
{

namespace
{
    constexpr float kChainFadeSeconds = 0.01f; // each way, when the order changes
}

void MasterChain::prepare (double sr, int maxBlockSize)
{
    sampleRate = (float) sr;
    for (auto& d : drives)
        d.prepare (sr, maxBlockSize);
    eq.prepare (sr);
    for (auto& c : comps)
        c.prepare (sr);
    for (auto& c : chainClippers)
        c.prepare (sr);

    outGain.reset (sr, 0.02);
    masterOut.reset (sr, 0.02);
    revLevel.reset (sr, 0.02);
    dryL.assign ((size_t) maxBlockSize, 0.0f);
    dryR.assign ((size_t) maxBlockSize, 0.0f);

    reverb.prepare (sr);
    outputFilter.prepare (sr);
    outComp.prepare (sr);
    clipper.prepare (sr);

    endDelaySize = (int) juce::nextPowerOfTwo (maxChainLatency() + 1);
    for (auto& d : endDelay)
        d.assign ((size_t) endDelaySize, 0.0f);

    fadeStep = 1.0f / (kChainFadeSeconds * sampleRate);
    reset();
}

void MasterChain::reset()
{
    firstBlock = true;
    for (auto& d : drives)
        d.reset();
    eq.reset();
    for (auto& c : comps)
        c.reset();
    for (auto& c : chainClippers)
        c.reset();
    reverb.reset();
    reverbCleared = false;
    outputFilter.reset();
    outComp.reset();
    clipper.reset();
    for (auto& d : endDelay)
        std::fill (d.begin(), d.end(), 0.0f);
    endDelayPos = 0;
    fade = Fade::none;
    fadeGain = 1.0f;
}

int MasterChain::moduleLatency (int module) const noexcept
{
    switch (fxKindOf (module))
    {
        case fxKindDrive: return drives[0].getLatencySamples();
        case fxKindClip:  return chainClippers[0].getLatencySamples();
        case fxKindEq: case fxKindComp: case fxKindReverb:
        default:          return 0;
    }
}

int MasterChain::getCompLeadSamples (int n) const noexcept
{
    int lead = clipper.getLatencySamples() + endDelaySamples;
    if (n < 0)
        return lead;
    const int module = n == 0 ? fxComp1 : fxComp2;
    for (int i = 0; i < active.length; ++i)
        if (active[i] == module)
        {
            for (int j = i + 1; j < active.length; ++j)
                lead += moduleLatency (active[j]);
            return lead;
        }
    return -1;
}

int MasterChain::chainLatency (const FxChain& c) const noexcept
{
    int total = 0;
    for (int i = 0; i < c.length; ++i)
        total += moduleLatency (c[i]);
    return total;
}

int MasterChain::maxChainLatency() const noexcept
{
    return 2 * drives[0].getLatencySamples() + 2 * chainClippers[0].getLatencySamples();
}

int MasterChain::getLatencySamples() const noexcept
{
    return maxChainLatency() + clipper.getLatencySamples();
}

void MasterChain::switchTo (const FxChain& next) noexcept
{
    // Every module starts clean in its new place (a filter's or an oversampler's state
    // belongs to the signal where it was; the chain is silent here anyway).
    for (int i = 0; i < next.length; ++i)
    {
        const int m = next[i];
        switch (m)
        {
            case fxDrive1: drives[0].reset(); break;
            case fxDrive2: drives[1].reset(); break;
            case fxEq:     eq.reset(); break;
            case fxComp1:  comps[0].reset(); break;
            case fxComp2:  comps[1].reset(); break;
            case fxClip1:  chainClippers[0].reset(); break;
            case fxClip2:  chainClippers[1].reset(); break;
            case fxReverb: reverb.reset(); reverbCleared = false; revLevel.setCurrentAndTargetValue (revLevel.getTargetValue()); break;
            default: break;
        }
    }
    active = next;
    endDelaySamples = juce::jlimit (0, endDelaySize - 1, maxChainLatency() - chainLatency (active));
}

void MasterChain::runReverb (float* L, float* R, int numSamples, const DrumParams& p, const int* hitOffsets, int numHits,
                             int latencyBefore) noexcept
{
    if (! reverbEnabled)
        return;

    if (revLevel.isSmoothing())
    {
        // Switching on or off: fade the wet signal (what the reverb adds) only.
        std::copy (L, L + numSamples, dryL.data());
        std::copy (R, R + numSamples, dryR.data());
        reverb.process (L, R, numSamples, p, hitOffsets, numHits, latencyBefore);
        for (int i = 0; i < numSamples; ++i)
        {
            const float w = revLevel.getNextValue();
            L[i] = dryL[(size_t) i] + (L[i] - dryL[(size_t) i]) * w;
            R[i] = dryR[(size_t) i] + (R[i] - dryR[(size_t) i]) * w;
        }
        reverbCleared = false;
    }
    else if (revLevel.getTargetValue() > 0.0f)
    {
        reverb.process (L, R, numSamples, p, hitOffsets, numHits, latencyBefore);
        reverbCleared = false;
    }
    else if (! reverbCleared)
    {
        // Off and faded out: drop the tail, so switching back on starts clean.
        reverb.reset();
        reverbCleared = true;
    }
}

void MasterChain::runModule (int module, float* L, float* R, int n, const DrumParams& p,
                             const int* hitOffsets, int numHits, int latencyBefore) noexcept
{
    switch (module)
    {
        case fxDrive1:
            drives[0].process (L, R, n, { p.driveOn, p.driveType, p.driveAmount, p.driveMix });
            break;
        case fxDrive2:
            drives[1].process (L, R, n, p.drive2);
            break;
        case fxEq:
            eq.process (L, R, n, p, p.eqOn);
            break;
        case fxComp1:
        case fxComp2:
        {
            const int k = module == fxComp1 ? 0 : 1;
            comps[k].process (L, R, n, p.comp[(size_t) k]);
            break;
        }
        case fxClip1:
        case fxClip2:
        {
            const int k = module == fxClip1 ? 0 : 1;
            const auto& c = p.chainClip[(size_t) k];
            const int mode = c.on ? Clipper::soft + juce::jlimit (0, chainClipShapeNames.size() - 1, c.shape) : Clipper::off;
            chainClippers[k].process (L, R, n, mode, c.driveDb, c.ceilingDb);
            break;
        }
        case fxReverb:
            runReverb (L, R, n, p, hitOffsets, numHits, latencyBefore);
            break;
        default:
            break;
    }
}

void MasterChain::process (juce::AudioBuffer<float>& buffer, int numSamples, const DrumParams& p,
                           const int* hitOffsets, int numHits) noexcept
{
    jassert (buffer.getNumChannels() >= 2);

    outGain.setTargetValue (p.outGain);
    masterOut.setTargetValue (p.masterOut);
    revLevel.setTargetValue (p.revOn ? 1.0f : 0.0f);

    if (firstBlock)
    {
        // Start from the current settings rather than ramping up from zero.
        firstBlock = false;
        for (auto* v : { &outGain, &masterOut, &revLevel })
            v->setCurrentAndTargetValue (v->getTargetValue());
        active = FxChain {};
        switchTo (p.chain);
        fade = Fade::none;
        fadeGain = 1.0f;
    }
    else if (p.chain != (fade == Fade::none ? active : pending))
    {
        // A new order: fade out (or keep fading out) towards it.
        pending = p.chain;
        fade = Fade::out;
    }

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    // --- The chain, in its order -----------------------------------------------------
    int latency = 0;
    for (int i = 0; i < active.length; ++i)
    {
        runModule (active[i], L, R, numSamples, p, hitOffsets, numHits, latency);
        latency += moduleLatency (active[i]);
    }

    if (fade != Fade::none)
    {
        // Out, then silence until what's still on its way out (the output stage's delay
        // and the end delay) has gone, so changing the end delay skips only silence;
        // then the switch, and in.
        for (int i = 0; i < numSamples; ++i)
        {
            L[i] *= fadeGain;
            R[i] *= fadeGain;
            if (fade == Fade::out)
            {
                fadeGain = juce::jmax (0.0f, fadeGain - fadeStep);
                if (fadeGain <= 0.0f)
                {
                    fade = Fade::hold;
                    holdLeft = clipper.getLatencySamples() + endDelaySize + 16;
                }
            }
            else if (fade == Fade::hold)
                --holdLeft;
            else
                fadeGain = juce::jmin (1.0f, fadeGain + fadeStep);
        }
        if (fade == Fade::hold && holdLeft <= 0)
        {
            switchTo (pending);
            fade = Fade::in;
        }
        else if (fade == Fade::in && fadeGain >= 1.0f)
            fade = Fade::none;
    }

    // --- Output low cut / high cut (untouched while both are off) -------------------
    if (outputFilterEnabled)
        outputFilter.process (L, R, numSamples, p);

    // --- Output gain, then the output stage (a Clipper, which delays by its latency even
    // when Off, or the output compressor and the Clipper's delay)
    for (int i = 0; i < numSamples; ++i)
    {
        const float og = outGain.getNextValue();
        L[i] *= og;
        R[i] *= og;
    }

    const bool compStage = p.clipMode == clipModeComp;
    if (preClip != nullptr)
    {
        const float stageGain = compStage ? juce::Decibels::decibelsToGain (p.outComp.makeupDb)
                              : p.clipMode == Clipper::off ? 1.0f
                                                           : juce::Decibels::decibelsToGain (p.clipDriveDb);
        const int n = juce::jmin (numSamples, preClip->getNumSamples() - preClipPos);
        for (int i = 0; i < n; ++i)
        {
            preClip->setSample (0, preClipPos + i, L[i] * stageGain);
            preClip->setSample (1, preClipPos + i, R[i] * stageGain);
        }
        preClipPos += juce::jmax (0, n);
    }

    auto oc = p.outComp;
    oc.on = compStage;
    outComp.process (L, R, numSamples, oc);
    clipper.process (L, R, numSamples, compStage ? (int) Clipper::off : p.clipMode, p.clipDriveDb, p.clipCeilingDb);

    // --- Master Out: the final volume, after the clipper (so it never changes how hard the
    // stage is driven); above 0 dB it can take the output past the ceiling
    for (int i = 0; i < numSamples; ++i)
    {
        const float mo = masterOut.getNextValue();
        L[i] *= mo;
        R[i] *= mo;
    }

    // --- The delay that stands in for modules not in the chain -----------------------
    const int mask = endDelaySize - 1;
    for (int i = 0; i < numSamples; ++i)
    {
        endDelay[0][(size_t) endDelayPos] = L[i];
        endDelay[1][(size_t) endDelayPos] = R[i];
        const int read = (endDelayPos - endDelaySamples) & mask;
        L[i] = endDelay[0][(size_t) read];
        R[i] = endDelay[1][(size_t) read];
        endDelayPos = (endDelayPos + 1) & mask;
    }
}

} // namespace drum
