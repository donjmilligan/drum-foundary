// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace drum
{

/** The compressor's characters: how its detector, timing and gain cell behave. Indices
    into compStyleNames (Parameters.h). Each is modelled on how a classic unit works
    (its topology and published behaviour), not on any other plug-in's code:

    - Clean: feed-forward, peak detector, gain smoothed in dB with the Attack and
      Release knobs (the original Drum Foundry compressor, untouched).
    - FET (1176-style): feedback (the detector hears the output), peak, a fast attack
      (the knob down to 25 microseconds), a firmer knee at high ratios, and odd
      harmonics that grow with the gain reduction, as a FET gain cell's do. The loop is
      solved exactly each sample (so it's stable at any attack), with its gain set so the
      steady-state ratio is the knob's, as the hardware's ratio buttons are; feedback
      shows in the timing: the harder it's pushed, the faster it clamps.
    - Opto (LA-2A-style): an optical cell: attack no faster than ~3 ms, and
      a two-stage release, half of it fast (half the Release knob) and half slow
      (several times it), slower still the longer and harder it has been compressing
      (the cell's memory). A wide soft knee, and a little even-harmonic warmth from the
      tube make-up stage.
    - Bus (SSL-style bus compressor): feed-forward VCA, peak, a firm knee, and an Auto
      release: a fast release for transients plus a slow one for sustained level, so it
      breathes with the groove instead of pumping. Clean gain cell.
    - Punch (dbx 160-style): feed-forward VCA, RMS detector (~5 ms), a wide "over easy"
      knee, attack that speeds up the harder it's hit (the knob's time at 10 dB over,
      a third of it at 30 dB) and a release at a constant rate in dB per second (120
      dB/s at a 120 ms Release), the punchy drum sound. A trace of odd harmonics. */
enum CompStyle
{
    compClean = 0,
    compFet,
    compOpto,
    compBus,
    compPunch,
    numCompStyles
};

/** Per-channel-pair state for the analog characters (Compressor holds one). */
struct CompModelState
{
    float gainDb = 0.0f;      // the gain change applied now (<= 0)
    float fastDb = 0.0f;      // Opto / Bus: the fast release stage
    float slowDb = 0.0f;      // Opto / Bus: the slow release stage
    float memory = 0.0f;      // Opto: how long and hard it has been compressing, 0..1
    float rmsSq = 0.0f;       // Punch: the RMS detector's mean square
    float dcL = 0.0f, dcR = 0.0f; // Opto: the even harmonics' DC, taken back out

    void reset() noexcept { *this = {}; }
};

namespace compmodel
{
    inline float dbOf (float linear) noexcept { return linear > 1.0e-6f ? 20.0f * std::log10 (linear) : -120.0f; }

    /** The static curve with a quadratic knee of `kneeDb` (the gain change, <= 0). */
    inline float curve (float inDb, float thresholdDb, float ratio, float kneeDb) noexcept
    {
        const float slope = 1.0f / juce::jmax (1.0f, ratio) - 1.0f;
        const float over = inDb - thresholdDb;
        if (kneeDb > 0.0f && std::abs (over) * 2.0f <= kneeDb)
        {
            const float t = over + 0.5f * kneeDb;
            return slope * t * t / (2.0f * kneeDb);
        }
        return over > 0.0f ? slope * over : 0.0f;
    }

    /** The static curve by slope (gain change per dB over, <= 0) instead of ratio. */
    inline float curveSlope (float inDb, float thresholdDb, float slope, float kneeDb) noexcept
    {
        const float over = inDb - thresholdDb;
        if (kneeDb > 0.0f && std::abs (over) * 2.0f <= kneeDb)
        {
            const float t = over + 0.5f * kneeDb;
            return slope * t * t / (2.0f * kneeDb);
        }
        return over > 0.0f ? slope * over : 0.0f;
    }

    /** The knee each character uses, from the Knee knob: FET firms up at high ratios,
        Opto and Punch are wide, Bus firm. */
    inline float kneeFor (int style, float kneeDb, float ratio) noexcept
    {
        switch (style)
        {
            case compFet:   return kneeDb * juce::jlimit (0.3f, 1.0f, 6.0f / juce::jmax (1.0f, ratio));
            case compOpto:  return juce::jmax (kneeDb, 12.0f);
            case compBus:   return juce::jmin (kneeDb, 3.0f);
            case compPunch: return juce::jmax (kneeDb, 10.0f);
            default:        return kneeDb;
        }
    }

    inline float coeff (float ms, double sr) noexcept
    {
        return std::exp (-1.0f / (juce::jmax (0.001f, ms) * 0.001f * (float) sr));
    }

    /** One sample of an analog character's gain computer and timing. `inPeak` is the
        louder channel's |x|, `inMeanSq` the mean of the channels' x^2. Returns the gain
        change in dB (<= 0) to apply to this sample. */
    inline float step (CompModelState& st, int style, float inPeak, float inMeanSq, float thresholdDb, float ratio,
                       float attackMs, float releaseMs, float kneeDb, double sr) noexcept
    {
        const float knee = kneeFor (style, kneeDb, ratio);
        switch (style)
        {
            case compFet:
            {
                // Feedback: the detector hears the output (input + this sample's gain change), and
                // the gain follows the curve of that through the attack / release smoothing:
                //   g = c g' + (1 - c) F (x + g),   F (y) = k (y - threshold) over it.
                // Solved for g (stable for any c and k <= 0). k = s / (1 + s) for the knob's slope s
                // makes the steady state g = s (x - threshold): the knob's ratio.
                const float x = dbOf (inPeak);
                const float s = 1.0f / juce::jmax (1.0f, ratio) - 1.0f;
                const float k = s / juce::jmax (0.05f, 1.0f + s);
                const float gp = st.gainDb;
                const bool attacking = curveSlope (x + gp, thresholdDb, k, knee) < gp;
                const float c = attacking ? coeff (juce::jmax (0.025f, attackMs * 0.25f), sr) : coeff (juce::jmax (5.0f, releaseMs), sr);
                const float linear = (c * gp + (1.0f - c) * k * (x - thresholdDb)) / (1.0f - (1.0f - c) * k);
                // Over the threshold that's exact; one more pass through the curve handles the knee
                // and below it (where F is 0 and the gain simply relaxes).
                st.gainDb = juce::jmin (0.0f, c * gp + (1.0f - c) * curveSlope (x + juce::jmin (0.0f, linear), thresholdDb, k, knee));
                return st.gainDb;
            }
            case compOpto:
            {
                // The cell's level, from the input (its steady state is the knob's ratio).
                const float target = curve (dbOf (inPeak), thresholdDb, ratio, knee);
                const float a = coeff (juce::jmax (3.0f, attackMs), sr);
                // The cell remembers: compressing charges it (over ~1 s), resting drains it (over ~5 s).
                const float depth = juce::jlimit (0.0f, 1.0f, -st.gainDb / 12.0f);
                st.memory += (depth - st.memory) * (depth > st.memory ? 1.0f / (float) sr : 0.2f / (float) sr);
                const float fastR = coeff (juce::jmax (5.0f, releaseMs * 0.5f), sr);
                const float slowR = coeff (juce::jmax (50.0f, releaseMs * (4.0f + 16.0f * st.memory)), sr);
                st.fastDb = target + (st.fastDb - target) * (target < st.fastDb ? a : fastR);
                st.slowDb = target + (st.slowDb - target) * (target < st.slowDb ? a : slowR);
                st.gainDb = 0.5f * (st.fastDb + st.slowDb);
                return st.gainDb;
            }
            case compBus:
            {
                const float target = curve (dbOf (inPeak), thresholdDb, ratio, knee);
                const float a = coeff (juce::jmax (0.1f, attackMs), sr);
                // Auto release: transients let go at a quarter of Release, sustained level at three times it.
                const float fastR = coeff (juce::jmax (5.0f, releaseMs * 0.25f), sr);
                const float slowR = coeff (juce::jmax (20.0f, releaseMs * 3.0f), sr);
                st.fastDb = target + (st.fastDb - target) * (target < st.fastDb ? a : fastR);
                // The slow stage only follows sustained reduction: it charges over ~100 ms more than
                // the attack, so a single hit barely moves it.
                const float slowA = coeff (100.0f + attackMs * 50.0f, sr);
                st.slowDb = target + (st.slowDb - target) * (target < st.slowDb ? slowA : slowR);
                st.gainDb = juce::jmin (st.fastDb, st.slowDb);
                return st.gainDb;
            }
            case compPunch:
            {
                // RMS over ~5 ms.
                const float w = 1.0f - coeff (5.0f, sr);
                st.rmsSq += (inMeanSq - st.rmsSq) * w;
                const float target = curve (0.5f * dbOf (st.rmsSq) * 1.0f, thresholdDb, ratio, knee);
                if (target < st.gainDb)
                {
                    // Faster the further over: the knob's time at 10 dB of reduction to come.
                    const float over = juce::jmax (1.0f, st.gainDb - target);
                    const float a = coeff (juce::jmax (0.2f, attackMs * juce::jlimit (0.33f, 3.0f, 10.0f / over)), sr);
                    st.gainDb = target + (st.gainDb - target) * a;
                }
                else
                {
                    // A constant rate in dB per second: 120 dB/s at Release 120 ms.
                    const float rate = 14400.0f / juce::jmax (5.0f, releaseMs) / (float) sr;
                    st.gainDb = juce::jmin (target, st.gainDb + rate);
                }
                return st.gainDb;
            }
            default:
                return 0.0f;
        }
    }

    /** The character's gain cell after the gain: FET's odd harmonics grow with the gain
        reduction, Opto adds a little even-harmonic warmth (its DC taken back out), Punch a
        trace of odd ones; Bus is clean. `y` is the compressed sample, `reductionDb` <= 0.
        The cell sees the signal relative to the threshold (`thresholdLin`, linear), as a real
        gain cell sees its working level, so the colour doesn't depend on the patch's volume. */
    inline float colour (int style, float y, float reductionDb, float thresholdLin, float& dc) noexcept
    {
        const float ref = juce::jmax (1.0e-4f, thresholdLin);
        const float z = y / ref;
        switch (style)
        {
            case compFet:
            {
                const float d = juce::jlimit (0.0f, 1.0f, -reductionDb / 20.0f) * 0.12f;
                return y + d * ref * (std::tanh (1.2f * z) / 1.2f - z);
            }
            case compOpto:
            {
                const float e = 0.012f * ref * z * z;
                dc += (e - dc) * 0.0005f;
                return y + e - dc;
            }
            case compPunch:
                return y + 0.02f * ref * (std::tanh (1.2f * z) / 1.2f - z);
            default:
                return y;
        }
    }
}

} // namespace drum
