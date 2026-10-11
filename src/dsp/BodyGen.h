// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "DspUtils.h"
#include "StyleInfo.h"
#include <cmath>

namespace drum
{

/** The synthesised body styles (Classic, Membrane, 808, 909, Kick, FM, the
    kick bodies Round, Taper, Hard, Fold and Tonal, and the basic waveforms Sine,
    Saw, Square, Round Square and Triangle): the
    Body layer's tonal drum head, and any extra layer whose source is Body.
    The Body layer's Sample style is played by the voice, not here.

    The style is fixed when the hit starts, and so are the parts of Shape and
    Head Modes that shape the strike (Membrane's mode tilt and tension glide,
    the 909's Snap, the 808's trigger pulse). Everything in Settings is read on
    every render() call. See StyleInfo.h for what the knobs mean per style.

    Pitch: `pitch` is the pitch curve in semitones over the base pitch. Every
    oscillator's frequency is kept between 0 Hz (it holds still) and 0.45 x the
    sample rate; partials that would pass that are muted, and the waveshaped
    styles (Classic's Shape, the 909's Drive, Round, Taper and Fold) fade to a
    sine as the pitch goes
    past ~1.5 kHz (at 48 kHz), so extreme curves stay alias-free. */
struct BodyGen
{
    static constexpr int numModes = 6;          // Classic head modes
    static constexpr int numMembraneModes = 14; // Membrane modes above (1,1)
    static constexpr int numHarmonics = 7;      // Tonal: harmonics 2..8

    /** Per-block settings. */
    struct Settings
    {
        float gain = 1.0f;          // linear level
        float shape = 0.1f;         // Shape / Strike / Punch / Drive / Index
        float otGain = 0.3f, otRatio = 1.594f, modes = 0.25f;
        float baseInc = 0.0f;       // the base pitch (Tune x key tracking), in cycles per sample
        float lengthSamples = 1.0f; // Length: the partials' decay times scale with it
    };

    /** Oscillator increment for `octaves` above `base` (cycles per sample), kept
        between 0 (the oscillator holds) and kMaxInc. */
    static float pitchInc (float base, float octaves) noexcept
    {
        return juce::jlimit (0.0f, dsp::kMaxInc, base * std::exp2 (octaves));
    }

    /** `shape` and `modes` are the Shape and Head Modes knobs at the hit, and
        `velocity` 0..1 (a harder hit glides further on Membrane). */
    void start (int bodyStyle, float shape, float modes, float velocity, float sr) noexcept
    {
        style = juce::jlimit (0, numBodyStyles - 1, bodyStyle);
        sampleRate = sr;

        phBody = phOvertone = 0.0f;
        otEnv = 1.0f;
        for (int m = 0; m < numModes; ++m)
        {
            phModes[m] = 0.0f;
            modeEnv[m] = kModeAmps[m];
        }

        // Membrane: a harder strike excites the upper modes more. Amplitudes are
        // normalised so Head Modes at 100% has the power of one full-scale sine.
        {
            const float tilt = 2.2f - 1.8f * shape;
            float power = 0.0f;
            for (int m = 0; m < numMembraneModes; ++m)
            {
                phMem[m] = 0.0f;
                memEnv[m] = std::pow (kMembraneRatios[m], -tilt);
                power += memEnv[m] * memEnv[m];
            }
            const float norm = 1.0f / std::sqrt (power);
            for (auto& e : memEnv)
                e *= norm;
            memLive = numMembraneModes;
        }

        // Built-in fast pitch drop, in octaves on top of the pitch curve: the head's
        // tension glide (harder and louder hits glide further), or the 909's Snap.
        glide = style == bodyMembrane ? (0.03f + 0.22f * shape) * velocity
              : style == body909      ? modes
                                      : 0.0f;

        resRe[0] = resRe[1] = resIm[0] = resIm[1] = 0.0f;
        resNorm[0] = resNorm[1] = 1.0f;
        pulseAge = 0;
        pulseLength = (0.00015f + 0.00185f * (1.0f - shape)) * sampleRate; // clamped on the first render
        resNormPending = true;

        fmIndexEnv = 1.0f;
        fmFb[0] = fmFb[1] = 0.0f;
        warmDc = 0.0f;
        phMod = 0.0f;
        driveEnv = 1.0f;

        for (int h = 0; h < numHarmonics; ++h)
        {
            phHarm[h] = 0.0f;
            harmEnv[h] = 1.0f;
        }

        // The basic waveforms start from 0 where they can: the saw mid-ramp; the square
        // can't (it fades in instead, see renderBasic()).
        basicAge = 0;
        triPeak = 0.5f + 0.45f * juce::jlimit (0.0f, 1.0f, shape);
        if (style == bodySaw)
            phBody = 0.5f;
    }

    /** Writes samples [from, n) of the body into `out` (mono), and the
        fundamental oscillator (what Wires coupling follows) into `couple`.
        `pitch` is the pitch curve in semitones and `level` the level envelope
        (up to 4, +12 dB; the FM index follows it up to 1), both indexed like `out`. */
    void render (int from, int n, const Settings& s, const float* pitch, const float* level, float* out, float* couple) noexcept
    {
        switch (style)
        {
            case bodyMembrane: renderMembrane (from, n, s, pitch, level, out, couple); break;
            case body808:      render808 (from, n, s, pitch, level, out, couple); break;
            case body909:      render909 (from, n, s, pitch, level, out, couple); break;
            case bodyKick:     renderKick (from, n, s, pitch, level, out, couple); break;
            case bodyFM:       renderFM (from, n, s, pitch, level, out, couple); break;
            case bodyRound:    renderRound (from, n, s, pitch, level, out, couple); break;
            case bodyTaper:    renderTaper (from, n, s, pitch, level, out, couple); break;
            case bodyHard:     renderHard (from, n, s, pitch, level, out, couple); break;
            case bodyFold:     renderFold (from, n, s, pitch, level, out, couple); break;
            case bodyTonal:    renderTonal (from, n, s, pitch, level, out, couple); break;
            case bodySine:
            case bodySaw:
            case bodySquare:
            case bodyRoundSquare:
            case bodyTriangle: renderBasic (from, n, s, pitch, level, out, couple); break;
            case bodyClassic:
            default:           renderClassic (from, n, s, pitch, level, out, couple); break;
        }
    }

    int style = bodyClassic;

private:
    // Circular membrane mode ratios relative to the fundamental (0,1) mode,
    // skipping (1,1) which the Overtone oscillator covers by default.
    static constexpr float kModeRatios[numModes] { 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f };
    static constexpr float kModeAmps[numModes]   { 1.0f, 0.8f, 0.6f, 0.5f, 0.4f, 0.3f };
    static constexpr float kModeNorm = 0.6f;

    // Membrane: the (1,1) mode, then the next 14 modes of an ideal circular
    // membrane (zeros of the Bessel functions over j01), in rising order so the
    // fastest-dying modes are last.
    static constexpr float kMembrane11 = 1.593f;
    static constexpr float kMembraneRatios[numMembraneModes] { 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f, 3.600f,
                                                               3.649f, 4.060f, 4.132f, 4.231f, 4.602f, 4.610f, 4.832f };
    static constexpr float kGlideSeconds = 0.005f; // Membrane tension glide and 909 Snap

    static constexpr float kOctavesPerSt = 1.0f / 12.0f;

    // Waveshaped oscillators (Classic's triangle / square, the 909's driven
    // triangles) are full strength up to this increment (1.44 kHz at 48 kHz)
    // and fade to a plain sine by kShapeFadeTo, so high pitches don't alias.
    static constexpr float kShapeFadeFrom = 0.03f;
    static constexpr float kShapeFadeTo = 0.08f;

    static float shapeFade (float inc) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (kShapeFadeTo - inc) / (kShapeFadeTo - kShapeFadeFrom));
    }

    static float shapeOscillator (float phase, float s, float shape, float squareDrive, float squareNorm) noexcept
    {
        if (shape < 0.001f)
            return s;

        const float tri = dsp::triangle01 (phase);
        if (shape <= 0.5f)
            return s + (tri - s) * (shape * 2.0f);

        const float sq = dsp::fastTanh (squareDrive * s) * squareNorm;
        return tri + (sq - tri) * ((shape - 0.5f) * 2.0f);
    }

    /** Raised-cosine trigger pulse for the 808 resonators, k = 0 .. length-1.
        Starts from exactly zero, so even a sharp pulse doesn't step. */
    static float pulseAt (int k, float length) noexcept
    {
        return 0.5f - 0.5f * dsp::fastSin01 ((float) k / length + 0.25f);
    }

    void renderClassic (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        const float shape = p.shape;
        const float squareDrive = 1.0f + juce::jmax (0.0f, shape - 0.5f) * 14.0f;
        const float squareNorm = 1.0f / dsp::fastTanh (squareDrive);

        // Higher partials die away faster than the fundamental, like a real head.
        const float lenF = p.lengthSamples;
        const float otDecay = std::exp (-1.0f / (0.3f * lenF));
        float modeDecay[numModes];
        for (int m = 0; m < numModes; ++m)
            modeDecay[m] = std::exp (-1.0f / (0.18f * lenF / (1.0f + 0.5f * (float) m)));

        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;
        const float modesGain = p.modes * kModeNorm;
        const bool doModes = modesGain > 0.0f;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            phBody += inc;
            phBody -= std::floor (phBody);

            // Shape fades back to a sine at high pitches (see kShapeFadeFrom).
            const float s = dsp::fastSin01 (phBody);
            const float sh = inc > kShapeFadeFrom ? shape * shapeFade (inc) : shape;
            const float osc = shapeOscillator (phBody, s, sh, squareDrive, squareNorm);

            // Partials that would pass kMaxInc are muted (but keep turning).
            phOvertone += inc * otRatio;
            phOvertone -= std::floor (phOvertone);
            const float ot = inc * otRatio < dsp::kMaxInc ? dsp::fastSin01 (phOvertone) * otEnv : 0.0f;
            otEnv *= otDecay;

            float modeSum = 0.0f;
            if (doModes)
            {
                for (int m = 0; m < numModes; ++m)
                {
                    phModes[m] += inc * kModeRatios[m];
                    phModes[m] -= std::floor (phModes[m]);
                    if (inc * kModeRatios[m] < dsp::kMaxInc)
                        modeSum += dsp::fastSin01 (phModes[m]) * modeEnv[m];
                    modeEnv[m] *= modeDecay[m];
                }
            }

            bodyL[i] = bodyGain * ctlBody[i] * (osc + otGain * ot + modesGain * modeSum);
            coupleSig[i] = osc;
        }
    }

    void renderMembrane (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // Mode m decays with a time constant of 0.3 x Length / ratio^1.5, so the
        // dense upper partials are gone within the first few tens of ms of a snare
        // while the fundamental follows the drawn envelope. Spread stretches every
        // mode's distance from the fundamental along with the (1,1) mode's ratio.
        const float tau = 0.3f * p.lengthSamples;
        const float otDecay = std::exp (-kMembrane11 * std::sqrt (kMembrane11) / tau);
        const float spread = (p.otRatio - 1.0f) / (kMembrane11 - 1.0f);

        float ratio[numMembraneModes], decay[numMembraneModes];
        for (int m = 0; m < memLive; ++m)
        {
            const float r = kMembraneRatios[m];
            ratio[m] = 1.0f + (r - 1.0f) * spread;
            decay[m] = std::exp (-r * std::sqrt (r) / tau);
        }

        const float glideDecay = std::exp (-1.0f / (kGlideSeconds * sampleRate));
        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;
        const float modesGain = p.modes;
        const bool doModes = modesGain > 0.0f;
        int live = memLive;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt + glide);
            glide *= glideDecay;

            const float fundamental = dsp::fastSin01 (phBody);
            phBody = dsp::wrap01 (phBody + inc);

            const float otInc = inc * otRatio;
            const float ot = otInc < dsp::kMaxInc ? dsp::fastSin01 (phOvertone) * otEnv : 0.0f;
            phOvertone = dsp::wrap01 (phOvertone + otInc);
            otEnv *= otDecay;

            // Modes keep decaying (and turning) while Head Modes is at 0, so turning
            // it up mid-hit doesn't bring them back at full strength.
            float modeSum = 0.0f;
            for (int m = 0; m < live; ++m)
            {
                const float mInc = inc * ratio[m];
                if (doModes && mInc < dsp::kMaxInc)
                    modeSum += dsp::fastSin01 (phMem[m]) * memEnv[m];
                phMem[m] = dsp::wrap01 (phMem[m] + mInc);
                memEnv[m] *= decay[m];
            }

            // Drop the highest mode once it has died away (the highest die first).
            // Done per sample so the result doesn't depend on the block size.
            if (live > 0 && memEnv[live - 1] < 1.0e-5f)
                --live;

            bodyL[i] = bodyGain * ctlBody[i] * (fundamental + otGain * ot + modesGain * modeSum);
            coupleSig[i] = fundamental;
        }
        memLive = live;
    }

    void render808 (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // Two damped resonators rung by a raised-cosine trigger pulse, like the
        // bridged-T circuits of an 808. The second decays twice as fast. At 0 Hz
        // a resonator stops turning and only decays (it stays stable at any pitch).
        const float decay1 = std::exp (-1.0f / (0.5f * p.lengthSamples));
        const float decay2 = std::exp (-1.0f / (0.25f * p.lengthSamples));
        const float otRatio = p.otRatio;

        if (resNormPending)
        {
            // The pulse is at most half a period of the higher resonator at its
            // starting pitch, which keeps both clear of the pulse spectrum's nulls.
            // Then scale each resonator so the pulse rings it up to ~unit amplitude.
            resNormPending = false;
            const float inc0 = pitchInc (p.baseInc, ctlPitch[from] * kOctavesPerSt);
            const float incs[2] { inc0, juce::jmin (dsp::kMaxInc, inc0 * otRatio) };
            const float highest = juce::jmax (1.0e-9f, incs[0], incs[1]); // (a 0 Hz start has no half period)
            pulseLength = juce::jlimit (2.0f, juce::jmax (2.0f, 0.5f / highest), pulseLength);
            const int pulseLen = (int) pulseLength;
            for (int r = 0; r < 2; ++r)
            {
                float re = 0.0f, im = 0.0f;
                for (int k = 0; k < pulseLen; ++k)
                {
                    const float x = pulseAt (k, pulseLength);
                    const float ph = incs[r] * (float) k;
                    re += x * dsp::fastSin01 (ph + 0.25f);
                    im -= x * dsp::fastSin01 (ph);
                }
                const float mag = std::sqrt (re * re + im * im);
                resNorm[r] = 1.0f / juce::jmax (mag, 0.125f * pulseLength);
            }
        }

        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float leak = 0.5f * p.shape * p.shape;
        const int pulseLen = (int) pulseLength;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float x = pulseAge < pulseLen ? pulseAt (pulseAge++, pulseLength) : 0.0f;

            float y[2];
            const float incs[2] { inc, juce::jmin (dsp::kMaxInc, inc * otRatio) };
            const float decays[2] { decay1, decay2 };
            for (int r = 0; r < 2; ++r)
            {
                const float c = decays[r] * dsp::fastSin01 (incs[r] + 0.25f);
                const float s = decays[r] * dsp::fastSin01 (incs[r]);
                const float re = c * resRe[r] - s * resIm[r] + x;
                const float im = s * resRe[r] + c * resIm[r];
                resRe[r] = re;
                resIm[r] = im;
                y[r] = im * resNorm[r];
            }

            bodyL[i] = bodyGain * ctlBody[i] * (y[0] + otGain * y[1] + leak * x);
            coupleSig[i] = y[0];
        }
    }

    void render909 (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // Two triangle oscillators, rounded or squared off by Drive, with Snap's
        // built-in pitch drop on top of the drawn curve. The second one dies faster.
        // Each fades to a sine at high pitches (see kShapeFadeFrom).
        const float drive = 1.0f + 4.0f * p.shape;
        const float driveNorm = 1.0f / dsp::fastTanh (drive);
        const float otDecay = std::exp (-1.0f / (0.2f * p.lengthSamples));
        const float glideDecay = std::exp (-1.0f / (kGlideSeconds * sampleRate));
        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;

        const auto osc = [drive, driveNorm] (float phase, float inc)
        {
            const float o = dsp::fastTanh (drive * dsp::triangle01 (phase)) * driveNorm;
            if (inc <= kShapeFadeFrom)
                return o;
            const float s = dsp::fastSin01 (phase);
            return s + (o - s) * shapeFade (inc);
        };

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt + glide);
            glide *= glideDecay;

            const float o1 = osc (phBody, inc);
            phBody = dsp::wrap01 (phBody + inc);

            const float otInc = juce::jmin (dsp::kMaxInc, inc * otRatio);
            const float o2 = osc (phOvertone, otInc) * otEnv;
            phOvertone = dsp::wrap01 (phOvertone + otInc);
            otEnv *= otDecay;

            bodyL[i] = bodyGain * ctlBody[i] * (o1 + otGain * o2);
            coupleSig[i] = o1;
        }
    }

    void renderKick (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // A sine from a zero crossing, swept by the drawn pitch curve. Drive adds
        // saturation that fades out (0.08 x Length + 10 ms), and fades as the
        // frequency rises too (whatever the curve), so a steep sweep stays
        // alias-free; Knock is a short partial.
        const float driveDecay = std::exp (-1.0f / (0.08f * p.lengthSamples + 0.01f * sampleRate));
        const float knockDecay = std::exp (-1.0f / (0.06f * p.lengthSamples));
        const float shape = p.shape;
        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float s = dsp::fastSin01 (phBody);
            phBody = dsp::wrap01 (phBody + inc);

            // Full drive up to 1% of the sample rate, none from 4%.
            const float amount = shape * driveEnv * juce::jlimit (0.0f, 1.0f, 1.0f - (inc - 0.01f) / 0.03f);
            driveEnv *= driveDecay;
            float y = s;
            if (amount > 1.0e-4f)
            {
                const float d = 1.0f + 8.0f * amount;
                const float sat = dsp::fastTanh (d * s) / dsp::fastTanh (d);
                y = s + (sat - s) * juce::jmin (1.0f, 4.0f * amount);
            }

            const float knock = dsp::fastSin01 (phOvertone) * otEnv;
            phOvertone = dsp::wrap01 (phOvertone + juce::jmin (dsp::kMaxInc, inc * otRatio));
            otEnv *= knockDecay;

            bodyL[i] = bodyGain * ctlBody[i] * (y + otGain * knock);
            coupleSig[i] = s;
        }
    }

    void renderFM (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // Two-operator phase modulation. The index (Shape x 8) follows the body
        // envelope up to 1 (above that the envelope only adds level), and Index
        // Decay makes it fall faster still. It is also capped so the sidebands stay
        // under 0.45 x the sample rate.
        constexpr float maxIndex = 8.0f;
        constexpr float invTwoPi = 1.0f / dsp::kTwoPi;
        const float index0 = maxIndex * p.shape;
        const float indexDecay = p.modes > 0.0f ? std::exp (-40.0f * p.modes / p.lengthSamples) : 1.0f;
        const float feedback = 0.25f * p.otGain; // in cycles
        const float otRatio = p.otRatio;
        const float bodyGain = p.gain;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float modInc = inc * otRatio;
            const float mod = dsp::fastSin01 (phMod + feedback * 0.5f * (fmFb[0] + fmFb[1]));
            fmFb[1] = fmFb[0];
            fmFb[0] = mod;

            // (A modulator standing still at 0 Hz makes no sidebands to limit.)
            const float limit = modInc > 0.0f ? juce::jmax (0.0f, (dsp::kMaxInc - inc) / modInc - 1.0f) : maxIndex;
            const float index = juce::jmin (limit, index0 * juce::jmin (1.0f, ctlBody[i]) * fmIndexEnv);
            fmIndexEnv *= indexDecay;

            const float carrier = dsp::fastSin01 (phBody + index * invTwoPi * mod);
            phMod = dsp::wrap01 (phMod + modInc);
            phBody = dsp::wrap01 (phBody + inc);
            bodyL[i] = bodyGain * ctlBody[i] * carrier;
            coupleSig[i] = carrier;
        }
    }

    //==============================================================================
    // Kick bodies. Each starts from a zero crossing (every shaper here is odd and
    // passes through 0), and the waveshaped ones fade to a sine at high pitches.

    /** Per-sample multiplier for the shape envelope of Taper and Fold: Head Modes
        0 holds it, 100% makes it fall with a time constant of 2% of Length. */
    static float shapeDecay (float modes, float lengthSamples) noexcept
    {
        if (modes <= 0.0f)
            return 1.0f;
        const float tau = (0.02f + 0.6f * (1.0f - modes) * (1.0f - modes)) * lengthSamples;
        return std::exp (-1.0f / juce::jmax (1.0f, tau));
    }

    /** tanh(d x) / tanh(d): rounds a unit sine's peaks off, keeping its peak at 1.
        For small d it is just x. */
    static float softSat (float x, float d) noexcept
    {
        if (d < 1.0e-3f)
            return x;
        return dsp::fastTanh (d * x) / dsp::fastTanh (d);
    }

    /** PolyBLEP: the correction that band-limits a unit step at phase 0, for an oscillator
        at increment `dt` (cycles per sample). */
    static float polyBlep (float t, float dt) noexcept
    {
        if (! (dt > 0.0f))
            return 0.0f;
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt)
        {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    /** The basic waveforms: Sine and Saw (Shape = Drive), Square (Shape = Width), Round
        Square (Shape = Corner) and Triangle (Shape = Skew; fixed at the hit). Saw and Square
        are band-limited with PolyBLEP; the shaped ones (the drives, Round Square, Triangle)
        fade to a sine as the pitch rises past ~1.5 kHz, as Classic's shapes do. */
    void renderBasic (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        const float shape = juce::jlimit (0.0f, 1.0f, p.shape);
        const float drive = 4.5f * shape;
        const float width = 0.5f - 0.4f * shape;            // Square's pulse width
        const float corner = 1.5f + 10.0f * shape;           // Round Square's drive
        const float cornerNorm = 1.0f / std::tanh (corner);
        const float rampSamples = 0.0005f * sampleRate;       // Square's fade-in
        const float bodyGain = p.gain;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float ph = phBody;
            const float s = dsp::fastSin01 (ph);
            const float fade = inc > kShapeFadeFrom ? shapeFade (inc) : 1.0f;
            float v = s;
            switch (style)
            {
                case bodySine:
                    v = softSat (s, drive * fade);
                    break;
                case bodySaw:
                {
                    const float saw = 1.0f - 2.0f * ph + polyBlep (ph, inc);
                    v = softSat (saw, drive * fade);
                    break;
                }
                case bodySquare:
                {
                    float sq = ph < width ? 1.0f : -1.0f;
                    sq += polyBlep (ph, inc);
                    sq -= polyBlep (dsp::wrap01 (ph + 1.0f - width), inc);
                    // Width moves the mean off 0: take it back out.
                    v = sq - (2.0f * width - 1.0f);
                    if ((float) basicAge < rampSamples)
                        v *= (float) basicAge / rampSamples;
                    ++basicAge;
                    break;
                }
                case bodyRoundSquare:
                {
                    const float rs = std::tanh (corner * s) * cornerNorm;
                    v = s + (rs - s) * fade;
                    break;
                }
                case bodyTriangle:
                default:
                {
                    // Rising from 0 at the hit: -1 -> 1 over triPeak of the cycle, back down over the rest.
                    const float u = dsp::wrap01 (ph + 0.5f * triPeak);
                    const float tri = u < triPeak ? -1.0f + 2.0f * u / triPeak : 1.0f - 2.0f * (u - triPeak) / (1.0f - triPeak);
                    v = s + (tri - s) * fade;
                    break;
                }
            }
            phBody = dsp::wrap01 (phBody + inc);
            bodyL[i] = bodyGain * ctlBody[i] * v;
            coupleSig[i] = s;
        }
    }

    void renderRound (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // A sine pushed into tanh: Round 0 is a pure sine, 100% a fat wave with
        // broad, rounded tops. Unlike Kick's Drive it doesn't fade with time, so the
        // tail stays as full as the head. Warmth adds the 2nd harmonic as an even
        // term (sine squared), which makes the wave fuller on one side, as tape or a
        // tube would, rather than tilting its tops. Squared, the sine starts from 0
        // like everything else; its mean (DC) is tracked at ~20 Hz and taken out.
        const float round = 4.5f * p.shape;
        const float bodyGain = p.gain;
        const float warmth = 2.0f * p.otGain;
        const float dcCoeff = 1.0f - std::exp (-dsp::kTwoPi * 20.0f / sampleRate);

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float s = dsp::fastSin01 (phBody);
            phBody = dsp::wrap01 (phBody + inc);

            // The square is a 2nd harmonic: muted (and its mean let go) past kMaxInc.
            const float sq = 2.0f * inc < dsp::kMaxInc ? s * s : 0.0f;
            warmDc += (sq - warmDc) * dcCoeff;

            const float d = inc > kShapeFadeFrom ? round * shapeFade (inc) : round;
            bodyL[i] = bodyGain * ctlBody[i] * (softSat (s, d) + warmth * (sq - warmDc));
            coupleSig[i] = s;
        }
    }

    void renderTaper (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // Point: sine -> triangle (0.5) -> a triangle raised to a power up to 2.5,
        // whose peaks are narrow spikes. Taper eases the point back to a sine over
        // the hit, so the head is thin and pointed and the tail is clean.
        const float decay = shapeDecay (p.modes, p.lengthSamples);
        const float knockDecay = std::exp (-1.0f / (0.06f * p.lengthSamples));
        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float s = dsp::fastSin01 (phBody);

            float point = p.shape * driveEnv;
            if (inc > kShapeFadeFrom)
                point *= shapeFade (inc);
            driveEnv *= decay;

            float y = s;
            if (point > 1.0e-4f)
            {
                const float tri = dsp::triangle01 (phBody);
                if (point <= 0.5f)
                    y = s + (tri - s) * (point * 2.0f);
                else
                {
                    const float k = 1.0f + 3.0f * (point - 0.5f); // 1 .. 2.5
                    y = std::copysign (std::pow (std::abs (tri), k), tri);
                }
            }
            phBody = dsp::wrap01 (phBody + inc);

            const float knock = dsp::fastSin01 (phOvertone) * otEnv;
            phOvertone = dsp::wrap01 (phOvertone + juce::jmin (dsp::kMaxInc, inc * otRatio));
            otEnv *= knockDecay;

            bodyL[i] = bodyGain * ctlBody[i] * (y + otGain * knock);
            coupleSig[i] = s;
        }
    }

    void renderHard (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // The hardstyle / gabber tail: Bark (a partial at Bark Ratio, the "EQ before
        // the distortion") is added to the sine, then up to ~30 dB of tanh, then
        // Crunch, a second clipping stage. The drive doesn't fade with time. It is
        // held under kHardCorner / (the highest frequency going in), which keeps the
        // clipped corners a few samples wide, so the sweep's high start clips less
        // and doesn't alias; past ~1.5 kHz the whole thing fades to the plain mix.
        constexpr float kHardCorner = 0.06f;
        const float drive = 30.0f * p.shape * std::sqrt (p.shape);
        const float crunch = 6.0f * p.modes;
        const float bark = p.otGain;
        const float barkRatio = p.otRatio;
        const float bodyGain = p.gain;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float barkInc = inc * barkRatio;
            const float s = dsp::fastSin01 (phBody);
            const float b = barkInc < dsp::kMaxInc ? dsp::fastSin01 (phOvertone) : 0.0f;
            phBody = dsp::wrap01 (phBody + inc);
            phOvertone = dsp::wrap01 (phOvertone + juce::jmin (dsp::kMaxInc, barkInc));

            const float x = s + bark * b;
            const float top = juce::jmax (inc, bark > 0.0f ? barkInc : 0.0f);
            const float limit = top > 0.0f ? kHardCorner / top : drive;
            const float y1 = softSat (x, juce::jmin (drive, limit));
            const float y2 = softSat (y1, juce::jmin (crunch, limit));
            const float y = inc > kShapeFadeFrom ? x + (y2 - x) * shapeFade (inc) : y2;

            bodyL[i] = bodyGain * ctlBody[i] * y;
            coupleSig[i] = s;
        }
    }

    void renderFold (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // A sine wavefolder: sin(pi/2 x k x sine), k from 1 to 5 (up to five folds),
        // blended in from a pure sine over the first quarter of Fold. Fold Decay
        // unfolds it over the hit; Knock as on Kick.
        const float decay = shapeDecay (p.modes, p.lengthSamples);
        const float knockDecay = std::exp (-1.0f / (0.06f * p.lengthSamples));
        const float bodyGain = p.gain;
        const float otGain = p.otGain;
        const float otRatio = p.otRatio;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float s = dsp::fastSin01 (phBody);
            phBody = dsp::wrap01 (phBody + inc);

            float fold = p.shape * driveEnv;
            if (inc > kShapeFadeFrom)
                fold *= shapeFade (inc);
            driveEnv *= decay;

            float y = s;
            if (fold > 1.0e-4f)
            {
                const float k = 1.0f + 4.0f * fold;
                const float folded = dsp::fastSin01 (0.25f * k * s); // sin (pi/2 k s)
                y = s + (folded - s) * juce::jmin (1.0f, 4.0f * fold);
            }

            const float knock = dsp::fastSin01 (phOvertone) * otEnv;
            phOvertone = dsp::wrap01 (phOvertone + juce::jmin (dsp::kMaxInc, inc * otRatio));
            otEnv *= knockDecay;

            bodyL[i] = bodyGain * ctlBody[i] * (y + otGain * knock);
            coupleSig[i] = s;
        }
    }

    void renderTonal (int from, int n, const Settings& p, const float* ctlPitch, const float* ctlBody, float* bodyL, float* coupleSig) noexcept
    {
        // The sine plus harmonics 2..8 at n^-tilt each, tilt from 2.5 (Bright 0)
        // to 0.5 (Bright 100%), normalised to the power of one full-scale sine.
        // Stretch moves harmonic n to n^log2(Stretch): x2 is exact. Harm Decay gives
        // harmonic n a time constant of 0.4 x Length / (1 + 4 x HarmDecay x (n - 1)).
        const float tilt = 2.5f - 2.0f * p.shape;
        const float stretch = std::log2 (juce::jmax (1.0f, p.otRatio));
        float amp[numHarmonics], ratio[numHarmonics], decay[numHarmonics];
        float power = 0.0f;
        for (int h = 0; h < numHarmonics; ++h)
        {
            const float num = (float) (h + 2);
            amp[h] = std::pow (num, -tilt);
            power += amp[h] * amp[h];
            ratio[h] = std::pow (num, stretch);
            decay[h] = p.modes > 0.0f ? std::exp (-(1.0f + 4.0f * p.modes * (num - 1.0f)) / (0.4f * p.lengthSamples)) : 1.0f;
        }
        const float norm = p.otGain / std::sqrt (power);
        for (auto& a : amp)
            a *= norm;

        const float bodyGain = p.gain;
        const bool doHarmonics = norm > 0.0f;

        for (int i = from; i < n; ++i)
        {
            const float inc = pitchInc (p.baseInc, ctlPitch[i] * kOctavesPerSt);
            const float s = dsp::fastSin01 (phBody);

            // Harmonics keep turning and decaying at Harmonics 0 (see Membrane).
            float sum = 0.0f;
            for (int h = 0; h < numHarmonics; ++h)
            {
                const float hInc = inc * ratio[h];
                if (doHarmonics && hInc < dsp::kMaxInc)
                    sum += dsp::fastSin01 (phHarm[h]) * amp[h] * harmEnv[h];
                phHarm[h] = dsp::wrap01 (phHarm[h] + juce::jmin (dsp::kMaxInc, hInc));
                harmEnv[h] *= decay[h];
            }
            phBody = dsp::wrap01 (phBody + inc);

            bodyL[i] = bodyGain * ctlBody[i] * (s + sum);
            coupleSig[i] = s;
        }
    }

    float sampleRate = 44100.0f;

    float phBody = 0, phOvertone = 0, otEnv = 1;
    float phModes[numModes] {}, modeEnv[numModes] {};

    float phMem[numMembraneModes] {}, memEnv[numMembraneModes] {};
    int memLive = numMembraneModes;             // modes from here up have died away
    float glide = 0;                            // Membrane / 909 built-in pitch drop, octaves, decaying

    float resRe[2] {}, resIm[2] {};             // 808 resonators
    float resNorm[2] { 1, 1 };
    float pulseLength = 2;                      // trigger pulse, samples
    int pulseAge = 0;
    bool resNormPending = true;

    float fmIndexEnv = 1, fmFb[2] {}, phMod = 0; // FM
    float driveEnv = 1;                          // Kick's drive, Taper's point, Fold's folding
    float phHarm[numHarmonics] {}, harmEnv[numHarmonics] {}; // Tonal
    float warmDc = 0;                            // Round: Warmth's tracked mean
    int basicAge = 0;                            // Square: samples since the hit (its fade-in)
    float triPeak = 0.5f;                        // Triangle: where its peak is in the cycle (Skew)
};

} // namespace drum
