// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "CompModels.h"
#include "FxChain.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace drum
{

/** A feed-forward, stereo-linked compressor for the master chain and the output
    stage. The detector is the louder channel's instantaneous peak, in dB; a soft-knee
    gain computer turns it into a gain change, which is smoothed in dB with separate
    attack and release times (branching, so a slow attack lets a drum's transient
    through and then clamps the body, and a fast one catches the transient). Makeup
    is added after, and Mix blends the compressed signal with the dry one (parallel
    compression), sample for sample (there's no look-ahead and no latency).

    That's the Clean style. The others (FET, Opto, Bus, Punch: CompModels.h) swap in a
    classic unit's detector, timing and gain cell, with the same knobs.

    Off and Mix glide over 20 ms. While fully off it leaves the signal untouched
    (bit for bit) and its detector rests, so switching it on starts clean.
    prepare() before use; process() doesn't allocate. */
class Compressor
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        wet.reset (sampleRate, 0.02);
        makeup.reset (sampleRate, 0.02);
        reset();
    }

    void reset() noexcept
    {
        gainDb = 0.0f;
        first = true;
        reductionDb = 0.0f;
        model.reset();
        capturePos = 0;
    }

    /** Offline renders only (not for the audio thread): while set, every sample appends
        the compressor's input (channel 0, mono), its output (1, mono, after Mix) and its
        gain change in dB (2, <= 0) to `buffer` (3 channels) until it is full. nullptr stops it. */
    void setCapture (juce::AudioBuffer<float>* buffer) noexcept
    {
        capture = buffer;
        capturePos = 0;
    }

    /** The static curve: the gain change (dB, <= 0) at an input level of `inDb`. */
    static float gainChangeDb (float inDb, float thresholdDb, float ratio, float kneeDb) noexcept
    {
        const float slope = 1.0f / juce::jmax (1.0f, ratio) - 1.0f; // <= 0
        const float over = inDb - thresholdDb;
        if (kneeDb > 0.0f && std::abs (over) * 2.0f <= kneeDb)
        {
            const float t = over + 0.5f * kneeDb;
            return slope * t * t / (2.0f * kneeDb);
        }
        return over > 0.0f ? slope * over : 0.0f;
    }

    /** What the compressor's output stands to the input at a steady level of `inDb`
        (gain change plus makeup, before Mix). For the UI's curve. */
    static float outputDb (float inDb, const CompSettings& s) noexcept
    {
        return inDb + gainChangeDb (inDb, s.thresholdDb, s.ratio, s.kneeDb) + s.makeupDb;
    }

    /** Processes a stereo pair in place. */
    void process (float* left, float* right, int numSamples, const CompSettings& s) noexcept
    {
        const float wetTarget = s.on ? juce::jlimit (0.0f, 1.0f, s.mix) : 0.0f;
        wet.setTargetValue (wetTarget);
        makeup.setTargetValue (juce::Decibels::decibelsToGain (s.makeupDb));
        if (first)
        {
            first = false;
            wet.setCurrentAndTargetValue (wetTarget);
            makeup.setCurrentAndTargetValue (makeup.getTargetValue());
        }

        reductionDb = 0.0f;
        if (! wet.isSmoothing() && wet.getTargetValue() <= 0.0f)
        {
            gainDb = 0.0f; // resting: off and faded out
            model.reset();
            makeup.skip (numSamples);
            captureBlock (left, right, left, right, nullptr, numSamples);
            return;
        }

        const int style = juce::jlimit (0, (int) numCompStyles - 1, s.style);
        if (style != compClean)
        {
            processModel (left, right, numSamples, s, style);
            return;
        }
        modelStyle = compClean;

        const float attack = std::exp (-1.0f / (juce::jmax (0.01f, s.attackMs) * 0.001f * (float) sr));
        const float release = std::exp (-1.0f / (juce::jmax (1.0f, s.releaseMs) * 0.001f * (float) sr));

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = left[i], r = right[i];
            const float peak = juce::jmax (std::abs (l), std::abs (r));
            const float inDb = peak > 1.0e-6f ? 20.0f * std::log10 (peak) : -120.0f;
            const float target = gainChangeDb (inDb, s.thresholdDb, s.ratio, s.kneeDb);

            // More reduction follows the attack time, less the release time.
            const float coeff = target < gainDb ? attack : release;
            gainDb = target + (gainDb - target) * coeff;
            reductionDb = juce::jmin (reductionDb, gainDb);

            const float g = std::exp (gainDb * 0.11512925f) * makeup.getNextValue(); // dB -> gain
            const float w = wet.getNextValue();
            left[i] = l + (l * g - l) * w;
            right[i] = r + (r * g - r) * w;
            captureSample (l, r, left[i], right[i], gainDb);
        }
    }

    /** The most gain reduction in the last block (dB, <= 0), for a meter. */
    float getReductionDb() const noexcept { return reductionDb; }

    /** The makeup gain it is applying now (linear): what a signal would come out at
        if nothing were compressed. */
    float getMakeupGain() const noexcept { return makeup.getCurrentValue(); }

private:
    /** An analog character (CompModels.h): its detector and timing, then its gain cell. */
    void processModel (float* left, float* right, int numSamples, const CompSettings& s, int style) noexcept
    {
        if (style != modelStyle)
        {
            // Changing character: carry the gain change over, so nothing jumps.
            model.reset();
            model.gainDb = model.fastDb = model.slowDb = gainDb;
            modelStyle = style;
        }
        const float thresholdLin = juce::Decibels::decibelsToGain (s.thresholdDb);
        for (int i = 0; i < numSamples; ++i)
        {
            const float l = left[i], r = right[i];
            const float peak = juce::jmax (std::abs (l), std::abs (r));
            const float meanSq = 0.5f * (l * l + r * r);
            gainDb = compmodel::step (model, style, peak, meanSq, s.thresholdDb, s.ratio, s.attackMs, s.releaseMs, s.kneeDb, sr);
            reductionDb = juce::jmin (reductionDb, gainDb);

            const float cell = std::exp (gainDb * 0.11512925f);
            const float mk = makeup.getNextValue();
            const float yl = compmodel::colour (style, l * cell, gainDb, thresholdLin, model.dcL) * mk;
            const float yr = compmodel::colour (style, r * cell, gainDb, thresholdLin, model.dcR) * mk;
            const float w = wet.getNextValue();
            left[i] = l + (yl - l) * w;
            right[i] = r + (yr - r) * w;
            captureSample (l, r, left[i], right[i], gainDb);
        }
    }

    void captureSample (float inL, float inR, float outL, float outR, float db) noexcept
    {
        if (capture == nullptr || capturePos >= capture->getNumSamples() || capture->getNumChannels() < 3)
            return;
        capture->setSample (0, capturePos, 0.5f * (inL + inR));
        capture->setSample (1, capturePos, 0.5f * (outL + outR));
        capture->setSample (2, capturePos, db);
        ++capturePos;
    }

    void captureBlock (const float* inL, const float* inR, const float* outL, const float* outR, const float*, int n) noexcept
    {
        if (capture == nullptr)
            return;
        for (int i = 0; i < n; ++i)
            captureSample (inL[i], inR[i], outL[i], outR[i], 0.0f);
    }

    double sr = 44100.0;
    float gainDb = 0.0f;
    float reductionDb = 0.0f;
    bool first = true;
    juce::SmoothedValue<float> wet, makeup;
    CompModelState model;
    int modelStyle = compClean;
    juce::AudioBuffer<float>* capture = nullptr; // see setCapture()
    int capturePos = 0;
};

} // namespace drum
