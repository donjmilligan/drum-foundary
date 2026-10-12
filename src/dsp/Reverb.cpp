// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "Reverb.h"

namespace drum
{

namespace
{
    // Line lengths at full Size. Irregularly spaced so their echoes don't line up.
    constexpr float kLineMs[Reverb::numLines] { 41.3f, 47.9f, 54.7f, 61.9f, 70.3f, 78.1f, 87.7f, 97.9f };
    constexpr float kMinScale = 0.12f; // Size 0 -> lines of ~5 to 12 ms (a small room)
    constexpr float kRefSize = 0.6f;   // Size's default: the level here is what it always was

    inline float lineScaleFor (float size) noexcept { return kMinScale + (1.0f - kMinScale) * size; }

    // Slow, unrelated LFO rates and a fraction-of-a-millisecond depth: enough to
    // smear the modes of the loop, too little to hear as pitch wobble.
    constexpr float kLfoHz[Reverb::numLines] { 0.31f, 0.43f, 0.57f, 0.71f, 0.83f, 0.97f, 1.13f, 1.29f };
    constexpr float kModDepthMs = 0.1f;

    // Input diffusion. The first kFixedDiffusers are short and fixed; the last
    // two ("room" diffusers, lengths given at full Size) scale with Size like
    // the lines, so a big room smears each reflection into a longer, denser
    // cloud (and builds up more slowly), while a small one stays tight. Without
    // them, the long lines of a big room let through a few discrete echoes
    // before the tail fills in.
    constexpr int kFixedDiffusers = 4;
    constexpr float kDiffMsL[Reverb::numDiffusers] { 1.31f, 2.09f, 3.17f, 4.79f, 6.83f, 11.29f };
    constexpr float kDiffMsR[Reverb::numDiffusers] { 1.43f, 2.27f, 3.41f, 5.03f, 7.51f, 12.07f };
    constexpr float kDiffGain[Reverb::numDiffusers] { 0.72f, 0.68f, 0.62f, 0.58f, 0.65f, 0.65f };

    constexpr float kHfRatio = 0.5f;   // RT60 at Nyquist, relative to the RT60 at DC
    constexpr float kReferenceHz = 2000.0f; // Decay is exact here
    constexpr float kInGain = 0.5f;    // into the FDN, per channel
    constexpr float kOutGain = 0.5f;   // out of it: at Mix 100% the wet is about as loud as the dry
    constexpr float kMaxLineGain = 0.99999f;

    constexpr int kTick = 32;          // control rate: smoothing, LFOs, coefficients
    constexpr float kSmoothMs = 50.0f;
    constexpr float kSendRampMs = 20.0f;
    constexpr float kQuiet = 1.0e-5f;  // -100 dB: below this the reverb may go idle
    constexpr float kCloudGain = 0.64f; // the Cloud engine's wet level, matched to Classic's (-3.9 dB; see checkReverbEngines)
    constexpr float kNaturalGain = 0.62f; // the Natural engine's, matched to Classic's (-4.2 dB; its rooms have unit energy; see checkReverbEngines)

    inline float finiteOr (float v, float fallback) noexcept { return std::isfinite (v) ? v : fallback; }

    void copyCoefficients (dsp::Svf& dest, const dsp::Svf& src) noexcept
    {
        dest.a1 = src.a1; dest.a2 = src.a2; dest.a3 = src.a3; dest.k = src.k;
    }

    /** In-place 8-point Hadamard transform, scaled to be orthonormal. */
    inline void hadamard8 (float* x) noexcept
    {
        for (int h = 1; h < 8; h *= 2)
            for (int i = 0; i < 8; i += 2 * h)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = x[j], b = x[j + h];
                    x[j] = a + b;
                    x[j + h] = a - b;
                }
        for (int i = 0; i < 8; ++i)
            x[i] *= 0.35355339f; // 1 / sqrt (8)
    }
}

//==============================================================================
void Reverb::DelayLine::allocate (int minLength)
{
    const int size = (int) juce::nextPowerOfTwo (juce::jmax (4, minLength));
    buffer.assign ((size_t) size, 0.0f);
    mask = size - 1;
    write = 0;
}

void Reverb::DelayLine::clear() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    write = 0;
}

//==============================================================================
void Reverb::prepare (double sr)
{
    prepared = false;
    if (! std::isfinite (sr) || sr <= 0.0)
        return;

    sampleRate = sr;
    const float msToSamples = (float) (sr * 0.001);

    modDepth = kModDepthMs * msToSamples;
    for (int i = 0; i < numLines; ++i)
    {
        lineBase[(size_t) i] = kLineMs[i] * msToSamples;
        lfoRate[(size_t) i] = (double) kLfoHz[i] / sr;
        lines[(size_t) i].allocate ((int) std::ceil (lineBase[(size_t) i] + modDepth) + 4);
    }

    maxPreDelay = maxPreDelayMs * msToSamples;
    preL.allocate ((int) std::ceil (maxPreDelay) + 4);
    preR.allocate ((int) std::ceil (maxPreDelay) + 4);

    float diffusion = 0.0f;
    for (int i = 0; i < numDiffusers; ++i)
    {
        auto setUp = [&] (Allpass& ap, float ms)
        {
            ap.length = i < kFixedDiffusers ? (float) juce::jmax (1, juce::roundToInt (ms * msToSamples)) : ms * msToSamples;
            ap.g = kDiffGain[i];
            ap.line.allocate ((int) std::ceil (ap.length) + 4);
            diffusion = juce::jmax (diffusion, ap.length);
        };
        setUp (diffL[(size_t) i], kDiffMsL[i]);
        setUp (diffR[(size_t) i], kDiffMsR[i]);
    }

    sendStep = 1.0f / (kSendRampMs * msToSamples);
    tickSmooth = 1.0f - std::exp (-(float) kTick / (kSmoothMs * msToSamples));
    attackStep = 1.0f / juce::jmax (1.0f, gateAttackMs * msToSamples);

    // Quiet for longer than anything can stay hidden in the buffers: the
    // pre-delay, the diffusers and the longest line.
    idleAfter = (int) std::ceil (maxPreDelay + (float) numDiffusers * diffusion + lineBase.back() + modDepth) + kTick;

    cloud.prepare (sr);
    natural.prepare (sr);
    reset();
    prepared = true;
}

void Reverb::clearBuffers() noexcept
{
    preL.clear();
    preR.clear();
    for (auto& l : lines)
        l.clear();
    for (auto& ap : diffL) ap.line.clear();
    for (auto& ap : diffR) ap.line.clear();
    damp.fill (0.0f);
    lowCutL.reset(); lowCutR.reset(); highCutL.reset(); highCutR.reset();
}

void Reverb::reset() noexcept
{
    clearBuffers();
    cloud.reset();
    cloudIdle = true;
    cloudQuiet = 0;
    natural.reset();
    naturalIdle = true;
    naturalQuiet = 0;
    idle = true;
    quietSamples = 0;
    clock = 0;
    resync = true;
    snapSend = true;

    gatePhase = GatePhase::closed;
    sinceHit = int64_t (1) << 40; // "a long time ago"
    gateGain = fadeFrom = 0.0f;
    fadePos = 0;
}

double Reverb::tailSeconds (const DrumParams& p) noexcept
{
    if (! p.revOn || ! (finiteOr (p.revMix, 0.0f) > 0.0f) || ! p.chain.contains (fxReverb))
        return 0.0;

    const double rt60 = juce::jlimit (0.05, 20.0, (double) finiteOr (p.revDecayMs, 2500.0f) * 0.001);
    const double pre = juce::jlimit (0.0, (double) maxPreDelayMs, (double) finiteOr (p.revPreDelayMs, 0.0f)) * 0.001;
    const double ungated = pre + 0.1 + 1.5 * rt60; // + the longest line before the tail starts
    if (! p.revGate)
        return ungated;

    const double gated = juce::jmax (0.0, (double) finiteOr (p.revGateMs, 180.0f) + (double) finiteOr (p.revFadeMs, 15.0f)) * 0.001;
    return juce::jmin (ungated, gated);
}

//==============================================================================
void Reverb::updateTargets (const DrumParams& p) noexcept
{
    const double msToSamples = sampleRate * 0.001;

    sendTarget    = juce::jlimit (0.0f, 1.0f, finiteOr (p.revMix, 0.0f));
    sizeTarget    = juce::jlimit (0.0f, 1.0f, finiteOr (p.revSize, kRefSize));
    rt60Target    = juce::jlimit (0.05f, 20.0f, finiteOr (p.revDecayMs, 2500.0f) * 0.001f);
    preTarget     = juce::jlimit (0.0f, maxPreDelay, finiteOr (p.revPreDelayMs, 0.0f) * (float) msToSamples);
    lowCutTarget  = juce::jlimit (10.0f, 24000.0f, finiteOr (p.revLowCut, 200.0f));
    highCutTarget = juce::jlimit (10.0f, 24000.0f, finiteOr (p.revHighCut, 8000.0f));

    gateOn      = p.revGate;
    gateSamples = (int) juce::jlimit (1.0, 1.0e8, std::round (finiteOr (p.revGateMs, 180.0f) * msToSamples));
    fadeSamples = (int) juce::jlimit (1.0, 1.0e7, std::round (finiteOr (p.revFadeMs, 15.0f) * msToSamples));
}

float Reverb::lfo (int line, int64_t atSample) const noexcept
{
    const double cycles = lfoRate[(size_t) line] * (double) atSample;
    return dsp::fastSin01 ((float) (cycles - std::floor (cycles)) + 0.125f * (float) line);
}

void Reverb::controlTick (bool snap) noexcept
{
    const int toNext = kTick - (int) (clock % kTick);

    if (snap)
    {
        sizeSmoothed = sizeTarget;
        logRt60 = std::log (rt60Target);
        preSmoothed = preTarget;
        logLowCut = std::log (lowCutTarget);
        logHighCut = std::log (highCutTarget);
    }
    else
    {
        sizeSmoothed += (sizeTarget - sizeSmoothed) * tickSmooth;
        logRt60 += (std::log (rt60Target) - logRt60) * tickSmooth;
        preSmoothed += (preTarget - preSmoothed) * tickSmooth;
        logLowCut += (std::log (lowCutTarget) - logLowCut) * tickSmooth;
        logHighCut += (std::log (highCutTarget) - logHighCut) * tickSmooth;
    }

    // Per-line gain for the RT60, and a damping pole that gives the RT60 at
    // Nyquist kHfRatio times that: gain g at DC, g * r at Nyquist, where the
    // one-pole's (1 - p) / (1 + p) = r. Both stay below 1, so the loop is
    // stable at any setting, even while they change.
    const float decayPerSample = -3.0f * 2.30258509f / (std::exp (logRt60) * (float) sampleRate);
    const float refCos = std::cos (dsp::kTwoPi * kReferenceHz / (float) sampleRate);
    const float scale = lineScaleFor (sizeSmoothed), refScale = lineScaleFor (kRefSize);

    // Level vs Size. With the same RT60, short lines go round the loop more
    // often, so without this a small room's tail carries far more energy than
    // a big one's (about +10 dB at Size 0 vs 1 with a 2.5 s Decay, +15 dB with
    // 0.4 s): turning Size up made the reverb quieter and sound smaller. The
    // output is scaled so the tail's energy at kReferenceHz, the sum over the
    // passes of g^2k = g^2 / (1 - g^2) per line, is what it is at kRefSize.
    const auto tailEnergy = [&] (float s)
    {
        float e = 0.0f;
        for (int i = 0; i < numLines; ++i)
        {
            const float g2 = juce::jmin (kMaxLineGain, std::exp (2.0f * lineBase[(size_t) i] * s * decayPerSample));
            e += g2 / (1.0f - g2);
        }
        return e;
    };
    const float out = kOutGain * std::sqrt (tailEnergy (refScale) / tailEnergy (scale));

    for (int i = 0; i < numLines; ++i)
    {
        const float base = lineBase[(size_t) i] * scale;
        const float end = base + modDepth * lfo (i, clock + toNext);
        lineCur[(size_t) i] = snap ? base + modDepth * lfo (i, clock) : lineEnd[(size_t) i];
        lineEnd[(size_t) i] = end;
        lineInc[(size_t) i] = (end - lineCur[(size_t) i]) / (float) toNext;

        const float r = std::exp (base * decayPerSample * (1.0f / kHfRatio - 1.0f));
        const float pole = (1.0f - r) / (1.0f + r);

        // Decay is the RT60 around kReferenceHz, where a snare's reverb has
        // most of its energy, so the DC gain makes up for the damping there.
        const float atRef = (1.0f - pole) / std::sqrt (1.0f - 2.0f * pole * refCos + pole * pole);
        const float g = juce::jmin (kMaxLineGain, std::exp (base * decayPerSample) / atRef);
        lineGain[(size_t) i] = g * (1.0f - pole);
        linePole[(size_t) i] = pole;
    }

    preCur = snap ? preSmoothed : preEnd;
    preEnd = preSmoothed;
    preInc = (preEnd - preCur) / (float) toNext;

    roomCur = snap ? scale : roomEnd;
    roomEnd = scale;
    roomInc = (roomEnd - roomCur) / (float) toNext;

    outCur = snap ? out : outEnd;
    outEnd = out;
    outInc = (outEnd - outCur) / (float) toNext;

    const float sr = (float) sampleRate;
    lowCutL.setCoefficients (std::exp (logLowCut), 0.7071f, sr);
    highCutL.setCoefficients (std::exp (logHighCut), 0.7071f, sr);
    copyCoefficients (lowCutR, lowCutL);
    copyCoefficients (highCutR, highCutL);

    samplesToTick = toNext;
}

//==============================================================================
float Reverb::nextGateGain() noexcept
{
    if (! gateOn)
    {
        // Off: open, ramping up in case it was shut when it was switched off.
        gateGain = juce::jmin (1.0f, gateGain + attackStep);
        gatePhase = GatePhase::open;
    }
    else
    {
        if (sinceHit == 0) // a hit reaches the reverb now
            gatePhase = gateGain >= 1.0f ? GatePhase::open : GatePhase::attack;

        switch (gatePhase)
        {
            case GatePhase::attack:
                gateGain += attackStep;
                if (gateGain >= 1.0f)
                {
                    gateGain = 1.0f;
                    gatePhase = GatePhase::open;
                }
                [[fallthrough]];

            case GatePhase::open:
                if (sinceHit >= gateSamples)
                {
                    gatePhase = GatePhase::fade;
                    fadePos = 0;
                    fadeFrom = gateGain;
                }
                break;

            case GatePhase::fade:
                if (++fadePos >= fadeSamples)
                {
                    gatePhase = GatePhase::closed;
                    gateGain = 0.0f;
                }
                else
                {
                    // Raised cosine: 0.5 * (1 + cos (pi * t))
                    const float t = (float) fadePos / (float) fadeSamples;
                    gateGain = fadeFrom * 0.5f * (1.0f + dsp::fastSin01 (0.25f + 0.5f * t));
                }
                break;

            case GatePhase::closed:
            default:
                break;
        }
    }

    ++sinceHit;
    return gateGain;
}

void Reverb::skipBlock (int n, const int* hitOffsets, int numHits, int hitDelay) noexcept
{
    // Idle: the output would be silent whatever the gate does, so just move the
    // gate to where it would be at the end of the block.
    if (numHits > 0)
        sinceHit = (int64_t) (n - hitOffsets[numHits - 1]) - hitDelay;
    else
        sinceHit += n;

    if (! gateOn)
    {
        gatePhase = GatePhase::open;
        gateGain = 1.0f;
    }
    else if (sinceHit >= 0)
    {
        if (sinceHit < gateSamples)
        {
            gatePhase = GatePhase::open;
            gateGain = 1.0f;
        }
        else if (sinceHit < (int64_t) gateSamples + fadeSamples)
        {
            gatePhase = GatePhase::fade;
            fadeFrom = 1.0f;
            fadePos = (int) (sinceHit - gateSamples);
            const float t = (float) fadePos / (float) fadeSamples;
            gateGain = 0.5f * (1.0f + dsp::fastSin01 (0.25f + 0.5f * t));
        }
        else
        {
            gatePhase = GatePhase::closed;
            gateGain = 0.0f;
        }
    }

    if (send < sendTarget)
        send = juce::jmin (sendTarget, send + sendStep * (float) n);
    else
        send = juce::jmax (sendTarget, send - sendStep * (float) n);

    clock += n;
    resync = true;
}

//==============================================================================
void Reverb::process (float* left, float* right, int n, const DrumParams& p,
                      const int* hitOffsets, int numHits, int hitDelay) noexcept
{
    if (! prepared || n <= 0)
        return;
    if (hitOffsets == nullptr)
        numHits = 0;

    updateTargets (p);
    if (snapSend)
    {
        send = sendTarget;
        snapSend = false;
    }

    // A new engine starts from silence (the old one's tail stops where it is). Cloud
    // keeps its own idle state, per sample (see processCloud()).
    const int nextEngine = juce::jlimit (0, revEngineNames.size() - 1, p.revEngine);
    if (nextEngine != engine)
    {
        engine = nextEngine;
        clearBuffers();
        cloud.reset();
        cloudIdle = true;
        cloudQuiet = 0;
        natural.reset();
        naturalIdle = true;
        naturalQuiet = 0;
        idle = true;
    }
    if (engine == revEngineCloud)
    {
        juce::ScopedNoDenormals noDenormals;
        processCloud (left, right, n, p, hitOffsets, numHits, hitDelay);
        return;
    }
    if (engine == revEngineNatural)
    {
        juce::ScopedNoDenormals noDenormals;
        processNatural (left, right, n, p, hitOffsets, numHits, hitDelay);
        return;
    }

    // Idle and nothing (audible) coming in: skip the whole reverb.
    if (idle)
    {
        bool silentInput = send <= 0.0f && sendTarget <= 0.0f;
        if (! silentInput)
        {
            const auto l = juce::FloatVectorOperations::findMinAndMax (left, n);
            const auto r = juce::FloatVectorOperations::findMinAndMax (right, n);
            const float peak = juce::jmax (-l.getStart(), l.getEnd(), -r.getStart(), r.getEnd());
            silentInput = peak * juce::jmax (send, sendTarget) < kQuiet;
        }
        if (silentInput)
        {
            skipBlock (n, hitOffsets, numHits, hitDelay);
            return;
        }
        idle = false;
    }

    juce::ScopedNoDenormals noDenormals;

    if (resync)
    {
        controlTick (true);
        resync = false;
    }

    float inPeak = 0.0f, wetPeak = 0.0f;
    int h = 0;

    for (int i = 0; i < n; ++i)
    {
        if (samplesToTick <= 0)
            controlTick (false);

        while (h < numHits && hitOffsets[h] <= i)
        {
            sinceHit = (int64_t) (i - hitOffsets[h]) - hitDelay;
            ++h;
        }
        const float gate = nextGateGain();

        if (! juce::exactlyEqual (send, sendTarget))
            send = std::abs (sendTarget - send) <= sendStep ? sendTarget
                                                            : send + (sendTarget > send ? sendStep : -sendStep);

        const float inL = left[i] * send, inR = right[i] * send;
        inPeak = juce::jmax (inPeak, std::abs (inL), std::abs (inR));

        // Pre-delay, then diffusion
        preL.push (inL);
        preR.push (inR);
        float xL = preL.read (preCur);
        float xR = preR.read (preCur);
        preCur += preInc;

        for (int a = 0; a < numDiffusers; ++a)
        {
            const float scaleBy = a < kFixedDiffusers ? 1.0f : roomCur;
            xL = diffL[(size_t) a].process (xL, diffL[(size_t) a].length * scaleBy);
            xR = diffR[(size_t) a].process (xR, diffR[(size_t) a].length * scaleBy);
        }
        roomCur += roomInc;

        // FDN: read (reading before writing, so a delay of D is read at D - 1),
        // damp, tap the outputs, mix and feed back.
        float y[numLines];
        for (int l = 0; l < numLines; ++l)
        {
            const float v = lines[(size_t) l].read (lineCur[(size_t) l] - 1.0f);
            lineCur[(size_t) l] += lineInc[(size_t) l];
            damp[(size_t) l] = lineGain[(size_t) l] * v + linePole[(size_t) l] * damp[(size_t) l];
            y[l] = damp[(size_t) l];
        }

        float wetL = outCur * ((y[0] - y[1]) + (y[2] - y[3]) + (y[4] - y[5]) + (y[6] - y[7]));
        float wetR = outCur * ((y[0] + y[1]) - (y[2] + y[3]) + (y[4] + y[5]) - (y[6] + y[7]));
        outCur += outInc;
        wetPeak = juce::jmax (wetPeak, std::abs (wetL), std::abs (wetR));

        hadamard8 (y);
        const float inA = kInGain * xL, inB = kInGain * xR;
        lines[0].push (y[0] + inA);
        lines[1].push (y[1] + inB);
        lines[2].push (y[2] - inA);
        lines[3].push (y[3] - inB);
        lines[4].push (y[4] + inA);
        lines[5].push (y[5] + inB);
        lines[6].push (y[6] - inA);
        lines[7].push (y[7] - inB);

        wetL = highCutL.lowpass (lowCutL.highpass (wetL));
        wetR = highCutR.lowpass (lowCutR.highpass (wetR));

        left[i] += wetL * gate;
        right[i] += wetR * gate;

        ++clock;
        --samplesToTick;
    }

    // Hits clamped to the very end of the block (the engine does that on its
    // last chunk) arrive after it.
    for (; h < numHits; ++h)
        sinceHit = (int64_t) (n - hitOffsets[h]) - hitDelay;

    // A NaN fails both comparisons, so a bad state never goes idle (and the
    // engine's output check then resets everything).
    if (inPeak < kQuiet && wetPeak < kQuiet)
        quietSamples += n;
    else
        quietSamples = 0;

    if (quietSamples >= idleAfter)
    {
        clearBuffers();
        idle = true;
        quietSamples = 0;
    }
}

//==============================================================================
void Reverb::processCloud (float* left, float* right, int n, const DrumParams& p,
                           const int* hitOffsets, int numHits, int hitDelay) noexcept
{
    // Cloud Seed makes the reverberation; the send, gate and wet cuts are the same as
    // Classic's. Everything here is per sample and on the reverb's own clock, so the
    // tail is the same whatever the host's block sizes: it goes idle (stops feeding
    // Cloud Seed, whose tail is under -100 dB by then, without clearing it) after
    // enough quiet, on a chunk boundary, and wakes on the first sample that sends
    // anything, with Cloud Seed's chunks where the clock puts them.
    CloudReverb::Settings s;
    s.size = juce::jlimit (0.0f, 1.0f, finiteOr (p.revSize, kRefSize));
    s.decaySeconds = juce::jlimit (0.05f, 20.0f, finiteOr (p.revDecayMs, 2500.0f) * 0.001f);
    s.preDelayMs = juce::jlimit (0.0f, maxPreDelayMs, finiteOr (p.revPreDelayMs, 0.0f));
    s.diffusion = finiteOr (p.revDiffusion, 0.7f);
    s.modDepth = finiteOr (p.revModDepth, 0.3f);
    s.modRate = finiteOr (p.revModRate, 0.3f);
    s.width = finiteOr (p.revWidth, 1.0f);
    s.early = finiteOr (p.revEarly, 0.2f);
    s.damping = finiteOr (p.revDamp, 0.4f);
    cloud.setSettings (s);

    // The cuts' coefficients are set once a block: they're on the wet signal only.
    lowCutL.setCoefficients (lowCutTarget, 0.707f, (float) sampleRate);
    copyCoefficients (lowCutR, lowCutL);
    highCutL.setCoefficients (highCutTarget, 0.707f, (float) sampleRate);
    copyCoefficients (highCutR, highCutL);

    const int64_t idleAfterSamples = juce::jmax ((int64_t) idleAfter, (int64_t) (sampleRate * (0.5 + s.preDelayMs * 0.001)));
    constexpr int chunk = CloudReverb::chunk;
    int h = 0;
    for (int i = 0; i < n; ++i, ++clock)
    {
        while (h < numHits && hitOffsets[h] <= i)
        {
            sinceHit = (int64_t) (i - hitOffsets[h]) - hitDelay;
            ++h;
        }
        const float gate = nextGateGain();

        if (! juce::exactlyEqual (send, sendTarget))
            send = std::abs (sendTarget - send) <= sendStep ? sendTarget
                                                            : send + (sendTarget > send ? sendStep : -sendStep);
        const float xL = left[i] * send, xR = right[i] * send;
        const bool loud = juce::jmax (std::abs (xL), std::abs (xR)) >= kQuiet;

        if (cloudIdle)
        {
            if (! loud)
                continue;
            cloudIdle = false;
            cloudQuiet = 0;
            cloud.resume ((int) (clock % chunk));
        }

        float wL = 0.0f, wR = 0.0f;
        cloud.processSample (xL, xR, wL, wR);
        wL = highCutL.lowpass (lowCutL.highpass (kCloudGain * wL));
        wR = highCutR.lowpass (lowCutR.highpass (kCloudGain * wR));
        left[i] += wL * gate;
        right[i] += wR * gate;

        cloudQuiet = loud || juce::jmax (std::abs (wL), std::abs (wR)) >= kQuiet ? 0 : cloudQuiet + 1;
        if (cloudQuiet >= idleAfterSamples && clock % chunk == chunk - 1)
        {
            lowCutL.reset(); lowCutR.reset(); highCutL.reset(); highCutR.reset();
            cloudIdle = true;
        }
    }

    for (; h < numHits; ++h)
        sinceHit = (int64_t) (n - hitOffsets[h]) - hitDelay;
    idle = cloudIdle;
}

//==============================================================================
void Reverb::processNatural (float* left, float* right, int n, const DrumParams& p,
                             const int* hitOffsets, int numHits, int hitDelay) noexcept
{
    // The room convolves chunks of the send (it's linear, so the tail doesn't depend on
    // the chunking); the send ramp, gate and wet cuts are per sample, as on Cloud. It goes
    // idle (stops feeding the convolver, without clearing it) once nothing has gone in
    // for longer than the room, so what's left in it is silence, and wakes on the first
    // sample that sends anything.
    NaturalReverb::Settings s;
    s.size = juce::jlimit (0.0f, 1.0f, finiteOr (p.revSize, kRefSize));
    s.decaySeconds = juce::jlimit (0.05f, 20.0f, finiteOr (p.revDecayMs, 2500.0f) * 0.001f);
    s.preDelayMs = juce::jlimit (0.0f, maxPreDelayMs, finiteOr (p.revPreDelayMs, 0.0f));
    s.diffusion = juce::jlimit (0.0f, 1.0f, finiteOr (p.revDiffusion, 0.7f));
    s.width = juce::jlimit (0.0f, 1.0f, finiteOr (p.revWidth, 1.0f));
    s.early = juce::jlimit (0.0f, 1.0f, finiteOr (p.revEarly, 0.2f));
    s.damping = juce::jlimit (0.0f, 1.0f, finiteOr (p.revDamp, 0.4f));
    natural.setSettings (s);

    lowCutL.setCoefficients (lowCutTarget, 0.707f, (float) sampleRate);
    copyCoefficients (lowCutR, lowCutL);
    highCutL.setCoefficients (highCutTarget, 0.707f, (float) sampleRate);
    copyCoefficients (highCutR, highCutL);

    const int64_t idleAfterSamples = juce::jmax ((int64_t) idleAfter, (int64_t) natural.getImpulseLength() + naturalChunk);
    int h = 0;
    for (int start = 0; start < n; start += naturalChunk)
    {
        const int len = juce::jmin (naturalChunk, n - start);
        bool loudIn = false;
        for (int i = 0; i < len; ++i)
        {
            while (h < numHits && hitOffsets[h] <= start + i)
            {
                sinceHit = (int64_t) (start + i - hitOffsets[h]) - hitDelay;
                ++h;
            }
            natGate[(size_t) i] = nextGateGain();

            if (! juce::exactlyEqual (send, sendTarget))
                send = std::abs (sendTarget - send) <= sendStep ? sendTarget
                                                                : send + (sendTarget > send ? sendStep : -sendStep);
            natInL[(size_t) i] = left[start + i] * send;
            natInR[(size_t) i] = right[start + i] * send;
            loudIn = loudIn || juce::jmax (std::abs (natInL[(size_t) i]), std::abs (natInR[(size_t) i])) >= kQuiet;
        }
        clock += len;

        if (naturalIdle)
        {
            if (! loudIn)
            {
                natural.serviceWhileIdle();
                continue;
            }
            naturalIdle = false;
            naturalQuiet = 0;
        }

        natural.process (natInL.data(), natInR.data(), natOutL.data(), natOutR.data(), len);
        bool loudOut = false;
        for (int i = 0; i < len; ++i)
        {
            const float wL = highCutL.lowpass (lowCutL.highpass (kNaturalGain * natOutL[(size_t) i]));
            const float wR = highCutR.lowpass (lowCutR.highpass (kNaturalGain * natOutR[(size_t) i]));
            loudOut = loudOut || juce::jmax (std::abs (wL), std::abs (wR)) >= kQuiet;
            left[start + i] += wL * natGate[(size_t) i];
            right[start + i] += wR * natGate[(size_t) i];
        }

        naturalQuiet = loudIn || loudOut ? 0 : naturalQuiet + len;
        if (naturalQuiet >= idleAfterSamples)
        {
            lowCutL.reset(); lowCutR.reset(); highCutL.reset(); highCutR.reset();
            naturalIdle = true;
        }
    }

    for (; h < numHits; ++h)
        sinceHit = (int64_t) (n - hitOffsets[h]) - hitDelay;
    idle = naturalIdle;
}

} // namespace drum
