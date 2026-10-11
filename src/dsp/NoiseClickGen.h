// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "DspUtils.h"
#include "SamplePlayer.h"
#include "StyleInfo.h"
#include <algorithm>

namespace drum
{

/** The noise source behind the Noise layer, and behind any extra layer whose
    source is Noise: every noise style, the low / high cut, Color's darkening,
    Width and the body coupling (see StyleInfo.h for Color and Grit per style).

    The style is fixed when the hit starts. Everything in Settings is read on
    every render() call.

    The noise pitch curve transposes the noise: Low and High Cut move together
    (as does Color's darkening), and so do Metallic's bank, Crackle's impulse
    rate and a sample's playback rate. Wires coupling and White / Pink's Crush
    stay as they are. It is followed every 16 samples. */
struct NoiseGen
{
    /** Per-block settings. */
    struct Settings
    {
        float gain = 1.0f;          // linear level, before the style's own make-up gain
        float lowCut = 450.0f, highCut = 9000.0f;
        float color = 0.4f, couple = 0.0f, width = 0.35f, grit = 0.0f;
        float pitchRatio = 1.0f;    // Metallic: transposes the bank (key tracking, and a layer's Tune)
        dsp::SampleView sample;     // Sample style only
        double sampleInc = 0.0;     // Sample style: source samples per output sample
    };

    // Filtered noise carries far less energy than a full-scale sine; this puts
    // the wires on a par with the body when both levels are at 0 dB.
    static constexpr float kNoiseGain = 2.8f;

    void start (int noiseStyle, uint32_t seed, float sr) noexcept
    {
        style = juce::jlimit (0, numNoiseStyles - 1, noiseStyle);
        sampleRate = sr;

        rngShared.seed (dsp::mixSeed (seed, 0x1234567u));
        rngLeft.seed (dsp::mixSeed (seed, 0x68E31DA4u));
        rngRight.seed (dsp::mixSeed (seed, 0xB5297A4Du));

        hpL.reset(); hpR.reset(); lpL.reset(); lpR.reset();
        lpCountdown = 0;

        for (int k = 0; k < 3; ++k)
            pinkL[k] = pinkR[k] = 0.0f;
        holdL = holdR = 0.0f;
        holdCount = 0;

        // Two metallic banks with unrelated start phases, so Width can spread them.
        dsp::Rng phases;
        phases.seed (dsp::mixSeed (seed, 0x5A17E3C1u));
        for (auto& bank : phMetal)
            for (auto& ph : bank)
                ph = phases.unipolar();

        smpPos = 0.0;
        pitchSt = 0.0f;
        pitchRatio = 1.0f;
    }

    /** Writes n samples into outL / outR. `env` is the level envelope, applied
        as it is (up to 4, +12 dB); Color's darkening follows it up to 1, so
        above 1 the noise is simply fully open. `body` is the body oscillator
        that Coupling modulates the noise by. `pitch` is the pitch curve in
        semitones (nullptr: none). */
    void render (int n, const Settings& s, const float* env, const float* pitch, const float* body,
                 float* outL, float* outR) noexcept
    {
        const bool isSample = style == noiseSample;

        // Sample keeps its own stereo image; the synthesised styles blend a shared
        // source with per-side ones by Width. Width only ever adds side (L - R):
        // the mid (L + R), which is what a mono fold-down or the phantom centre
        // gets, stays at the Width 0 level. Each side is shared * (1 - w) + own * w,
        // so the mid's power is (1 - w)^2 + w^2 / 2 of one source's; mixNorm
        // undoes that (x1 at Width 0, up to +3 dB per side at 100%, where the two
        // sides are uncorrelated). Metallic's two banks already keep their sum
        // whatever the Width, so they need no make-up.
        const float width = isSample ? 0.0f : s.width;
        const float mixNorm = 1.0f / std::sqrt ((1.0f - width) * (1.0f - width) + 0.5f * width * width);
        const float widthNorm = style == noiseMetallic ? 1.0f : mixNorm;
        const float grit = s.grit;
        const float couple = s.couple;

        float styleGain = kNoiseGain;
        switch (style)
        {
            case noisePink:     styleGain = kNoiseGain * kPinkGain; break;
            case noiseMetallic: styleGain = kNoiseGain * kMetalGain; break;
            case noiseSample:   styleGain = 1.0f; break;
            default:            break;
        }
        const float noiseGain = s.gain * styleGain;

        const auto setLowCut = [this, &s]
        {
            hpL.setCoefficients (s.lowCut * pitchRatio, 0.707f, sampleRate);
            hpR.a1 = hpL.a1; hpR.a2 = hpL.a2; hpR.a3 = hpL.a3; hpR.k = hpL.k;
        };
        setLowCut();
        const float lpFloor = juce::jmax (200.0f, s.lowCut * 1.2f);

        // Metallic uses Color for the bank's pitch, so its tail doesn't darken.
        const float color = style == noiseMetallic ? 0.0f : s.color;

        // White / Pink: Grit is a sample-and-hold "crush" of 1 to 32 samples.
        const int holdLength = 1 + (int) (grit * grit * 31.0f);

        // Metallic: six band-limited squares per bank, Color = x0.35 .. x2.8 of 205 Hz.
        // A square transposed too high (key tracking, a layer's Tune) is faded
        // out between 0.1 and 0.15 x the sample rate, where PolyBLEP stops
        // keeping its harmonics from aliasing.
        float metalInc[6] {}, metalGain[6] {};
        const float metalBase = style == noiseMetallic ? kMetalBaseHz * s.pitchRatio * std::exp2 ((s.color - 0.5f) * 3.0f) / sampleRate
                                                       : 0.0f;
        const auto setMetal = [&]
        {
            const float base = metalBase * pitchRatio;
            for (int k = 0; k < 6; ++k)
            {
                metalInc[k] = juce::jmin (dsp::kMaxInc, base * kMetalRatios[k]);
                metalGain[k] = juce::jlimit (0.0f, 1.0f, (kMetalMaxInc - metalInc[k]) / kMetalFadeInc);
            }
        };
        if (style == noiseMetallic)
            setMetal();
        const float metalMix = 0.5f - 0.5f * width; // how much of the other bank each side gets

        // Crackle: Grit = density, 100 .. 6400 impulses per second on each source.
        float crackleProb = 0.0f, crackleGain = 0.0f;
        const auto setCrackle = [&]
        {
            crackleProb = juce::jmin (0.5f, 100.0f * std::exp2 (grit * 6.0f) * pitchRatio / sampleRate);
            crackleGain = juce::jmin (kCrackleMaxGain, 1.0f / std::sqrt (crackleProb));
        };
        if (style == noiseCrackle)
            setCrackle();

        const auto& view = s.sample;
        const double smpRate = isSample ? s.sampleInc : 0.0;

        for (int i = 0; i < n; ++i)
        {
            float sl = 0.0f, sr = 0.0f;
            switch (style)
            {
                case noiseWhite:
                case noisePink:
                {
                    if (--holdCount <= 0)
                    {
                        holdCount = holdLength;
                        const float w0 = rngShared.bipolar();
                        const float wl = rngLeft.bipolar();
                        const float wr = rngRight.bipolar();
                        holdL = w0 + (wl - w0) * width;
                        holdR = w0 + (wr - w0) * width;
                    }
                    sl = holdL;
                    sr = holdR;

                    if (style == noisePink)
                    {
                        // Paul Kellet's economy pink filter.
                        const auto pink = [] (float* b, float w)
                        {
                            b[0] = 0.99765f * b[0] + w * 0.0990460f;
                            b[1] = 0.96300f * b[1] + w * 0.2965164f;
                            b[2] = 0.57000f * b[2] + w * 1.0526913f;
                            return b[0] + b[1] + b[2] + w * 0.1848f;
                        };
                        sl = pink (pinkL, sl);
                        sr = pink (pinkR, sr);
                    }
                    break;
                }

                case noiseMetallic:
                {
                    float bank[2] {};
                    for (int b = 0; b < 2; ++b)
                        for (int k = 0; k < 6; ++k)
                        {
                            phMetal[b][k] = dsp::wrap01 (phMetal[b][k] + metalInc[k]);
                            bank[b] += dsp::blepSquare (phMetal[b][k], metalInc[k]) * metalGain[k];
                        }

                    sl = bank[0] + (bank[1] - bank[0]) * metalMix;
                    sr = bank[1] + (bank[0] - bank[1]) * metalMix;

                    if (grit > 0.0f)
                    {
                        // Blend in white noise (scaled to the bank's RMS).
                        const float w0 = rngShared.bipolar() * 4.24f;
                        sl += ((w0 + (rngLeft.bipolar() * 4.24f - w0) * width) * mixNorm - sl) * grit;
                        sr += ((w0 + (rngRight.bipolar() * 4.24f - w0) * width) * mixNorm - sr) * grit;
                    }
                    break;
                }

                case noiseCrackle:
                {
                    const auto dust = [p = crackleProb, g = crackleGain] (dsp::Rng& rng)
                    {
                        const float u = rng.unipolar();
                        return u < p ? (2.0f * u / p - 1.0f) * g : 0.0f;
                    };
                    const float w0 = dust (rngShared);
                    sl = w0 + (dust (rngLeft) - w0) * width;
                    sr = w0 + (dust (rngRight) - w0) * width;
                    break;
                }

                case noiseSample:
                {
                    if (! view.isFinished (smpPos))
                    {
                        view.read (smpPos, sl, sr);
                        smpPos += smpRate * (double) pitchRatio;
                    }
                    break;
                }

                case noiseWires:
                default:
                {
                    float w0 = rngShared.bipolar();
                    float wl = rngLeft.bipolar();
                    float wr = rngRight.bipolar();

                    // Grit is picked from each raw (uniform) source before the width
                    // mix, so impulse density doesn't depend on the Width setting.
                    if (grit > 0.0f)
                    {
                        auto gritify = [grit] (float w)
                        {
                            const float g = std::abs (w) > kGritThreshold ? w * kGritGain : 0.0f;
                            return w + (g - w) * grit;
                        };
                        w0 = gritify (w0);
                        wl = gritify (wl);
                        wr = gritify (wr);
                    }

                    sl = w0 + (wl - w0) * width;
                    sr = w0 + (wr - w0) * width;
                    break;
                }
            }

            // Wires rattle against the bottom head: modulate them by the body.
            const float am = (1.0f + couple * body[i]) * widthNorm;
            sl *= am;
            sr *= am;

            if (--lpCountdown <= 0)
            {
                lpCountdown = kLpUpdateInterval;

                // The pitch curve: every filter and rate that follows it moves together.
                if (pitch != nullptr && ! juce::exactlyEqual (pitch[i], pitchSt))
                {
                    pitchSt = pitch[i];
                    pitchRatio = dsp::transposeRatio (pitchSt);
                    setLowCut();
                    if (style == noiseMetallic)
                        setMetal();
                    else if (style == noiseCrackle)
                        setCrackle();
                }

                const float cutoff = juce::jmax (lpFloor, s.highCut * std::exp2 (-color * (1.0f - juce::jmin (1.0f, env[i])) * 6.0f))
                                   * pitchRatio;
                lpL.setCoefficients (cutoff, 0.707f, sampleRate);
                lpR.a1 = lpL.a1; lpR.a2 = lpL.a2; lpR.a3 = lpL.a3; lpR.k = lpL.k;
            }

            const float ng = noiseGain * env[i];
            outL[i] = lpL.lowpass (hpL.highpass (sl)) * ng;
            outR[i] = lpR.lowpass (hpR.highpass (sr)) * ng;
        }
    }

    int style = noiseWires;

private:
    // 808 cymbal / hat oscillator ratios (205.3, 304.4, 369.6, 522.7, 540, 800 Hz).
    static constexpr float kMetalRatios[6] { 1.0f, 1.4827f, 1.8003f, 2.5460f, 2.6303f, 3.8967f };
    static constexpr float kMetalBaseHz = 205.3f;
    static constexpr float kMetalMaxInc = 0.15f, kMetalFadeInc = 0.05f;

    // Sparse "grit" impulses are the loudest ~4% of white noise samples,
    // scaled so their RMS roughly matches the white noise they replace.
    static constexpr float kGritThreshold = 0.96f;
    static constexpr float kGritGain = 2.9f;

    // Per noise style, on top of kNoiseGain: brings each source to roughly the
    // RMS of the uniform white noise that Wires uses (1 / sqrt 3).
    static constexpr float kPinkGain = 0.11f;
    static constexpr float kMetalGain = 0.2357f;   // 1/sqrt(3) over sqrt(6) squares
    static constexpr float kCrackleMaxGain = 4.0f; // caps single impulses at low densities

    static constexpr int kLpUpdateInterval = 16;

    float sampleRate = 44100.0f;
    float pitchSt = 0.0f, pitchRatio = 1.0f;     // the pitch curve where it was last followed
    dsp::Rng rngShared, rngLeft, rngRight;
    dsp::Svf hpL, hpR, lpL, lpR;
    int lpCountdown = 0;
    float pinkL[3] {}, pinkR[3] {};
    float holdL = 0, holdR = 0;
    int holdCount = 0;
    float phMetal[2][6] {};
    double smpPos = 0.0;
};

//==============================================================================
/** The physically modelled click types (Felt, Plastic, Skin, Wood, Tok): what a
    beater or stick hitting something sounds like, rather than a single synthetic
    spike. Modelled on measurements of ~600 recorded and produced kick clicks and
    rims (`drumfoundry-tools measure-clicks`), which differ from a synthetic click in
    the same ways: a contact that lasts 0.2-2 ms (softer = longer = darker), a couple
    of bounces and early reflections that make 5-10 small impulses and a 1-3 ms rise,
    a resonance or two standing ~10 dB out of the spectrum, highs that die well before
    the lows (so it darkens as it rings), and a low thump under it.

    The model: an excitation made of raised-cosine force pulses (the contact, its
    bounces, and a few reflections of each, timed and weighted afresh on every hit),
    rings a bank of damped modes (the main resonance at Tone, inharmonic modes spread
    around it, and a thump at a fixed pitch), plus a noise texture that the same
    pulses drive, low-passed from bright to dark as it decays. Each material is a set
    of numbers (kMaterials). Every hit varies a little (mode tuning, levels, contact,
    reflection times), as a real player's would; a given seed always gives the same hit.

    Tone moves the main resonance and every Tone-relative mode, and a higher Tone also
    hardens the contact. Length scales every decay (8 ms = the material as measured).
    The click pitch curve transposes all of it. */
struct ImpactGen
{
    static constexpr int kNumMaterials = 5;           // clickFelt .. clickTok
    static constexpr int kMaxModes = 14;              // inharmonic modes + main resonance + thump
    static constexpr int kMaxEvents = 24;             // force pulses: contacts x (1 + reflections)

    struct Material
    {
        float refTone;                                // the Tone the numbers are for (its default)
        float contactMs;                              // force pulse width at refTone
        int bounces; float bounceMs, bounceGain;      // extra contacts after the first
        int taps; float tapMinMs, tapMaxMs, tapGain;  // early reflections of each contact
        int modes; float modeLo, modeHi, tilt;        // modes log-spread over [lo, hi] x Tone, level (f / Tone)^-tilt
        float mainGain, mainDecay;                    // the resonance at Tone, and its T60 relative to `decayMs`
        float decayMs, decayAlpha;                    // T60 of a mode at Tone; x (f / Tone)^-alpha
        float noiseGain, noiseCut0, noiseCut1, noiseGlideMs, noiseDecayMs; // texture: low-pass cut0 -> cut1 (x Tone)
        float strike;                                 // the impact's own snap: bright texture at the contact, gone in ~0.6 ms
        float thumpHz, thumpGain, thumpDecayMs;
        float outGain;
        uint32_t layout;                              // seed for the material's fixed mode layout
    };

    static const Material& material (int index) noexcept
    {
        static const Material m[kNumMaterials] {
            // Felt: a soft kick beater on a head. A knock that rings on at Tone (~0.9 kHz), a bright
            // snap on top, darkening fast, and a low thump.
            { 900.0f,  1.0f,  1, 1.4f, 0.8f,   4, 0.3f, 3.0f, 0.6f,  10, 0.35f, 5.0f, 0.6f,  3.0f, 2.0f,  30.0f, 0.8f,
              0.6f, 8.0f, 0.5f, 2.5f, 6.0f, 1.5f,  110.0f, 1.2f, 180.0f,  0.7f, 0x0F17u },
            // Plastic: a hard beater. Short contact, a bright "ting" at Tone over a low knock.
            { 7000.0f, 0.12f, 1, 1.2f, 0.8f,   4, 0.3f, 2.5f, 0.6f,  12, 0.06f, 1.6f, 0.4f,  3.0f, 1.2f,  25.0f, 0.4f,
              0.4f, 2.0f, 0.08f, 2.0f, 6.0f, 0.5f,  140.0f, 1.6f, 120.0f,  1.0f, 0x9A57u },
            // Skin: a miked acoustic head, top end only. One clear snap, then the head and room ring on bright.
            { 4000.0f, 0.5f,  2, 1.6f, 0.25f,  4, 0.4f, 5.0f, 0.2f,  12, 0.3f, 2.2f, 0.0f,  1.5f, 1.5f,  50.0f, 0.6f,
              0.15f, 2.0f, 0.7f, 30.0f, 30.0f, 3.0f,  0.0f, 0.0f, 0.0f,  3.5f, 0x51C3u },
            // Wood: a stick on a rim or block. Hard, mid-centred, every band rings about as long;
            // the stick lands twice (rim, then head), the second time harder.
            { 1500.0f, 0.35f, 2, 1.8f, 1.15f,  4, 0.3f, 4.0f, 0.6f,  10, 0.5f, 6.0f, 0.4f,  4.0f, 1.3f,  45.0f, 0.1f,
              0.2f, 8.0f, 1.5f, 10.0f, 20.0f, 1.8f,   0.0f, 0.0f, 0.0f,  0.7f, 0x3D0Du },
            // Tok: the hardstyle "tok". A pitched, resonant knock at Tone with few modes, ringing long.
            { 2200.0f, 0.6f,  1, 1.5f, 0.5f,   2, 0.3f, 1.5f, 0.7f,   6, 0.5f, 3.0f, 1.0f,  4.0f, 1.5f, 600.0f, 1.0f,
              0.35f, 3.0f, 1.0f, 8.0f, 150.0f, 0.8f,  0.0f, 0.0f, 0.0f,  1.5f, 0x70C5u },
        };
        return m[juce::jlimit (0, kNumMaterials - 1, index)];
    }

    void start (int materialIndex, float toneHz, float lengthMs, uint32_t seed, float sr) noexcept
    {
        const auto& m = material (materialIndex);
        sampleRate = sr;
        age = 0;
        active = true;

        dsp::Rng hit, layout;
        hit.seed (dsp::mixSeed (seed, 0x6A09E667u));
        layout.seed (m.layout);

        const float tone = juce::jlimit (100.0f, sr * 0.45f, toneHz);
        const float lenScale = juce::jmax (0.05f, lengthMs / 8.0f);
        const float ms = 0.001f * sr;

        // Contact: harder (shorter) as Tone rises above the material's own, +-15% per hit.
        const float contact = juce::jmax (2.0f, m.contactMs * ms * std::sqrt (m.refTone / tone) * (1.0f + 0.15f * hit.bipolar()));

        // Force pulses: the contact and its bounces, each with its own reflections.
        float tapAt[8] {}, tapAmp[8] {};
        const int taps = juce::jmin (m.taps, 8);
        for (int t = 0; t < taps; ++t)
        {
            const float u = ((float) t + 0.5f + 0.45f * hit.bipolar()) / (float) taps;
            tapAt[t] = (m.tapMinMs + (m.tapMaxMs - m.tapMinMs) * u * u) * ms;
            tapAmp[t] = m.tapGain * (0.55f + 0.45f * hit.unipolar()) * (1.0f - 0.5f * u) * (hit.unipolar() < 0.3f ? -1.0f : 1.0f);
        }
        numEvents = 0;
        lastEventEnd = 0;
        for (int b = 0; b <= m.bounces && numEvents < kMaxEvents; ++b)
        {
            const float at = b == 0 ? 0.0f : (float) b * m.bounceMs * ms * (1.0f + 0.25f * hit.bipolar());
            const float amp = b == 0 ? 1.0f : std::pow (m.bounceGain, (float) b) * (0.8f + 0.4f * hit.unipolar());
            const float width = contact * (b == 0 ? 1.0f : 0.8f);
            addEvent (at, amp, width);
            for (int t = 0; t < taps && numEvents < kMaxEvents; ++t)
                addEvent (at + tapAt[t], amp * tapAmp[t], width * (1.0f + 0.3f * (float) (t + 1)));
        }

        // Modes: the fixed layout of the material, detuned a little on each hit.
        const float common = 1.0f + 0.006f * hit.bipolar();
        numModes = 0;
        float longest = 0.0f;
        const auto addMode = [&] (float hz, float amp, float t60Ms)
        {
            if (numModes >= kMaxModes || amp <= 0.0f || hz <= 0.0f)
                return;
            const int k = numModes++;
            modeHz[k] = hz;
            modeAmp[k] = amp;
            const float t60 = juce::jlimit (0.5f, 1500.0f, t60Ms * lenScale); // a click rings 2 s at most
            modeR[k] = std::exp (-6.9078f / (t60 * ms));
            modeRe[k] = modeIm[k] = 0.0f;
            longest = juce::jmax (longest, t60);
        };
        addMode (tone * common, m.mainGain * (0.85f + 0.3f * hit.unipolar()), m.decayMs * m.mainDecay);
        addMode (m.thumpHz * common, m.thumpGain, m.thumpDecayMs);
        const int count = juce::jmin (m.modes, kMaxModes - 2);
        for (int k = 0; k < count; ++k)
        {
            const float u = ((float) k + 0.5f + 0.35f * layout.bipolar()) / (float) count;
            const float ratio = m.modeLo * std::pow (m.modeHi / m.modeLo, u);
            const float level = std::pow (ratio, -m.tilt) * juce::Decibels::decibelsToGain (3.0f * layout.bipolar() + 1.5f * hit.bipolar());
            addMode (tone * ratio * common * (1.0f + 0.003f * hit.bipolar()), level, m.decayMs * std::pow (ratio, -m.decayAlpha));
        }

        // The modes start together, so their levels are scaled to sum to 1: the bank
        // can't go past ~1 however many modes a material has.
        float sum = 0.0f;
        for (int k = 0; k < numModes; ++k)
            sum += modeAmp[k];
        for (int k = 0; k < numModes; ++k)
            modeAmp[k] /= juce::jmax (1.0e-6f, sum);

        // Texture: noise that the pulses drive, low-passed from bright to dark.
        noiseRng.seed (dsp::mixSeed (seed, 0xBB67AE85u));
        noiseGain = m.noiseGain;
        noiseEnv = 0.0f;
        strikeEnv = m.strike;
        strikeMul = std::exp (-1.0f / (0.6f * ms));
        const float noiseTau = juce::jlimit (0.1f, 375.0f, m.noiseDecayMs * lenScale); // gone (-35 dB) by the end
        noiseMul = std::exp (-1.0f / (noiseTau * ms));
        glide = 1.0f;
        glideMul = std::exp (-1.0f / (juce::jmax (0.1f, m.noiseGlideMs * lenScale) * ms));
        cut0 = m.noiseCut0 * tone;
        cut1 = m.noiseCut1 * tone;
        noiseFilter.reset();
        longest = juce::jmax (longest, 4.0f * noiseTau); // ms; with the caps above, a click rings 2.1 s at most

        outGain = m.outGain;
        pitchRatio = 1.0f;
        filterCountdown = 0;
        endAge = lastEventEnd + (int) (1.4f * longest * ms) + 1;
        setRotations();
    }

    /** Transposes everything by `ratio` (the click pitch curve). */
    void retune (float ratio) noexcept
    {
        pitchRatio = ratio;
        setRotations();
        filterCountdown = 0;
    }

    float next() noexcept
    {
        // Excitation: the sum of the raised-cosine pulses under way (each has unit area).
        float x = 0.0f;
        for (int e = 0; e < numEvents; ++e)
        {
            const int k = age - evStart[e];
            if (k >= 0 && k < evLen[e])
                x += evAmp[e] * (1.0f - dsp::fastSin01 ((float) k / (float) evLen[e] + 0.25f)) / (float) evLen[e];
        }

        float y = 0.0f;
        for (int k = 0; k < numModes; ++k)
        {
            const float re = modeC[k] * modeRe[k] - modeS[k] * modeIm[k] + x;
            const float im = modeS[k] * modeRe[k] + modeC[k] * modeIm[k];
            modeRe[k] = re;
            modeIm[k] = im;
            y += modeAmp[k] * im;
        }

        noiseEnv = noiseEnv * noiseMul + std::abs (x);
        const float drive = noiseEnv + strikeEnv;
        strikeEnv *= strikeMul;
        if (--filterCountdown <= 0)
        {
            filterCountdown = 16;
            const float cut = (cut1 + (cut0 - cut1) * glide) * pitchRatio;
            noiseFilter.setCoefficients (juce::jlimit (20.0f, 0.45f * sampleRate, cut), 0.6f, sampleRate);
        }
        glide *= glideMul;
        y += noiseGain * noiseFilter.lowpass (noiseRng.bipolar() * drive);

        if (++age >= endAge)
            active = false;
        return outGain * y;
    }

    bool active = false;

private:
    float sampleRate = 44100.0f;
    int age = 0, endAge = 0, lastEventEnd = 0;

    int numEvents = 0;
    int evStart[kMaxEvents] {}, evLen[kMaxEvents] {};
    float evAmp[kMaxEvents] {};

    int numModes = 0;
    float modeHz[kMaxModes] {}, modeAmp[kMaxModes] {}, modeR[kMaxModes] {};
    float modeC[kMaxModes] {}, modeS[kMaxModes] {}, modeRe[kMaxModes] {}, modeIm[kMaxModes] {};

    dsp::Rng noiseRng;
    dsp::Svf noiseFilter;
    float noiseGain = 0, noiseEnv = 0, noiseMul = 0, strikeEnv = 0, strikeMul = 0, glide = 0, glideMul = 0, cut0 = 0, cut1 = 0;
    int filterCountdown = 0;
    float outGain = 1.0f, pitchRatio = 1.0f;

    void addEvent (float atSamples, float amp, float widthSamples) noexcept
    {
        const int e = numEvents++;
        evStart[e] = (int) std::round (atSamples);
        evLen[e] = juce::jmax (2, (int) std::round (widthSamples));
        evAmp[e] = amp;
        lastEventEnd = juce::jmax (lastEventEnd, evStart[e] + evLen[e]);
    }

    /** Each mode turns by its frequency x pitchRatio and decays by its radius.
        A mode at or past kMaxInc is muted (it rings no more while it is up there). */
    void setRotations() noexcept
    {
        for (int k = 0; k < numModes; ++k)
        {
            const float inc = modeHz[k] * pitchRatio / sampleRate;
            const float r = inc < dsp::kMaxInc ? modeR[k] : 0.0f;
            modeC[k] = r * dsp::fastSin01 (inc + 0.25f);
            modeS[k] = r * dsp::fastSin01 (inc);
        }
    }
};

//==============================================================================
/** The Click layer's synthesised transients (Tick .. Tok), also used by any
    extra layer whose source is Click. Each type decays on its own over Length.
    Felt .. Tok are played by ImpactGen.
    The Sample type's state lives here too, but the voice plays it.

    The click pitch curve (follow()) transposes the click: every frequency of
    the type (Tone's filter, the Stick / Rim / Pop / Beater oscillators) and a
    sample's playback rate move by 2^(st/12), followed every 16 samples. */
struct ClickGen
{
    /** `toneHz` and `lengthMs` are the Tone and Length knobs. The type, tone and
        length are fixed for the hit. */
    void start (int clickType, float toneHz, float lengthMs, bool on, uint32_t seed, float sr) noexcept
    {
        type = juce::jlimit (0, numClickTypes - 1, clickType);
        active = on;
        if (isImpact (type))
        {
            impact.start (type - clickFelt, toneHz, lengthMs, seed, sr);
            pitchSt = 0.0f;
            pitchRatio = 1.0f;
            pitchCountdown = 0;
            return;
        }
        env = 1.0f;
        phase = phase2 = 0.0f;
        rng.seed (dsp::mixSeed (seed, 0x1B56C4E9u));

        const float lenSamples = juce::jmax (1.0f, lengthMs * 0.001f * sr);
        // Length is roughly the time to fall ~36 dB. Tick decays twice as fast.
        const float tau = lenSamples / (type == clickTick ? 8.0f : 4.0f);
        envMul = std::exp (-1.0f / tau);

        const float tone = juce::jlimit (100.0f, sr * 0.45f, toneHz);
        inc = tone / sr;
        inc2 = inc * 1.47f;
        sampleRate = sr;
        baseTone = tone;
        pitchSt = 0.0f;
        pitchRatio = 1.0f;
        pitchCountdown = 0;

        filter.reset();
        filterR.reset();
        switch (type)
        {
            case clickTick: filter.setCoefficients (tone, 0.707f, sr); break;        // high-passed impulse noise
            case clickSnap: filter.setCoefficients (tone, 1.1f, sr); break;          // band-passed noise
            case clickRim:  filter.setCoefficients (tone * 2.0f, 2.0f, sr); break;   // noise component
            case clickBeater:
                // Low-passed click at Tone, plus a knock whose pitch follows Tone.
                filter.setCoefficients (tone, 0.707f, sr);
                inc = (60.0f + 0.06f * tone) / sr;
                break;
            case clickClap:
            {
                // 3 or 4 band-passed noise bursts 8-12 ms apart (random per hit,
                // from the seed), then a wider band-passed tail from the last
                // one that falls ~36 dB over 5 x Length (2.5 to 200 ms).
                filter.setCoefficients (tone, 1.4f, sr);
                filterR.setCoefficients (tone * 0.85f, 0.9f, sr);
                dsp::Rng spacing;
                spacing.seed (dsp::mixSeed (seed, 0x2F6B1C3Du));
                clapBursts = spacing.unipolar() < 0.5f ? 3 : 4;
                int at = 0;
                for (int b = 0; b < clapBursts; ++b)
                {
                    clapBurstAt[b] = at;
                    clapBurstAmp[b] = b == clapBursts - 1 ? 1.0f : 0.65f + 0.3f * spacing.unipolar();
                    at += (int) ((0.008f + 0.004f * spacing.unipolar()) * sr);
                }
                clapNext = 0;
                burstEnv = 0.0f;
                burstMul = std::exp (-1.0f / (kClapBurstSeconds * sr));
                env = 0.0f;
                envMul = std::exp (-1.0f / (1.25f * lenSamples));
                age = 0;
                break;
            }
            case clickSample:
                // Tone is a low cut on the sample; all the way down means off.
                highPass = toneHz > 300.5f;
                filter.setCoefficients (tone, 0.707f, sr);
                filterR = filter;
                smpPos = 0.0;
                age = 0;
                maxLength = (int) lenSamples;
                fadeLength = juce::jmax (1, (int) juce::jmin (0.002f * sr, 0.3f * lenSamples));
                break;
            default: break;
        }
        baseInc = inc;
        baseInc2 = inc2;
    }

    /** Follows the click pitch curve (`st` semitones at this sample): call once
        per sample, before next() or the Sample type's read. The click is
        retuned every 16 samples, and only when the curve has moved. */
    void follow (float st) noexcept
    {
        if (--pitchCountdown > 0)
            return;
        pitchCountdown = kPitchUpdateInterval;
        if (juce::exactlyEqual (st, pitchSt))
            return;
        pitchSt = st;
        pitchRatio = dsp::transposeRatio (st);
        retune();
    }

    /** The next sample of a synthesised type (not Sample). Call only while active. */
    float next() noexcept
    {
        if (type == clickClap)
            return nextClap();
        if (isImpact (type))
        {
            const float v = impact.next();
            active = impact.active;
            return v;
        }

        float v = 0.0f;
        switch (type)
        {
            case clickTick: v = filter.highpass (rng.bipolar()) * 1.2f; break;
            case clickSnap: v = filter.bandpass (rng.bipolar()) * 2.0f; break;
            case clickStick:
                phase += inc;
                v = dsp::fastSin01 (phase);
                break;
            case clickRim:
                phase += inc;
                phase2 += inc2;
                v = 0.55f * dsp::fastSin01 (phase) + 0.35f * dsp::fastSin01 (phase2)
                  + 0.6f * filter.bandpass (rng.bipolar());
                break;
            case clickBeater:
                // Beater: a knock that falls a little in pitch, plus a low-passed
                // click that dies twice as fast.
                phase += inc * (0.6f + 0.4f * env);
                v = 0.8f * dsp::fastSin01 (phase) + 1.6f * env * filter.lowpass (rng.bipolar());
                break;
            case clickPop:
            default:
                // Pop: a sine that sweeps down two octaves as it decays.
                phase += inc * (0.25f + 0.75f * env);
                v = dsp::fastSin01 (phase);
                break;
        }

        v *= env;
        env *= envMul;
        if (env < 1.0e-4f)
            active = false;
        return v;
    }

    int type = 0;
    bool active = false;
    float pitchRatio = 1.0f;                     // the pitch curve's ratio, where last followed
    float env = 0, envMul = 0;                   // Clap: the tail
    float phase = 0, phase2 = 0, inc = 0, inc2 = 0;
    dsp::Svf filter, filterR;
    dsp::Rng rng;

    // Sample type (age: Clap too)
    double smpPos = 0.0;
    int age = 0, maxLength = 1, fadeLength = 1;
    bool highPass = false;

    // Clap
    static constexpr int kMaxClapBursts = 4;
    int clapBursts = 0, clapNext = 0;
    int clapBurstAt[kMaxClapBursts] {};
    float clapBurstAmp[kMaxClapBursts] {};
    float burstEnv = 0, burstMul = 0;

    ImpactGen impact;                            // Felt .. Tok

    static constexpr bool isImpact (int t) noexcept { return t >= clickFelt && t <= clickTok; }

private:
    static constexpr float kClapBurstSeconds = 0.0025f; // each burst's decay time constant
    static constexpr float kClapTailLevel = 0.45f;
    static constexpr int kPitchUpdateInterval = 16;

    float sampleRate = 44100.0f;
    float baseTone = 1000.0f, baseInc = 0, baseInc2 = 0;  // as started: Tone (limited) and the oscillators
    float pitchSt = 0.0f;
    int pitchCountdown = 0;

    /** Sets the type's frequencies to its start ones x pitchRatio. Filter states
        are kept, so a moving curve doesn't click. */
    void retune() noexcept
    {
        if (isImpact (type))
        {
            impact.retune (pitchRatio);
            return;
        }
        const float tone = juce::jlimit (10.0f, sampleRate * 0.45f, baseTone * pitchRatio);
        inc = juce::jmin (dsp::kMaxInc, baseInc * pitchRatio);
        inc2 = juce::jmin (dsp::kMaxInc, baseInc2 * pitchRatio);
        switch (type)
        {
            case clickTick:   filter.setCoefficients (tone, 0.707f, sampleRate); break;
            case clickSnap:   filter.setCoefficients (tone, 1.1f, sampleRate); break;
            case clickRim:    filter.setCoefficients (tone * 2.0f, 2.0f, sampleRate); break;
            case clickBeater: filter.setCoefficients (tone, 0.707f, sampleRate); break;
            case clickClap:
                filter.setCoefficients (tone, 1.4f, sampleRate);
                filterR.setCoefficients (tone * 0.85f, 0.9f, sampleRate);
                break;
            default: break;   // Stick and Pop are oscillators only; Sample's Tone is its low cut, which stays
        }
    }

    float nextClap() noexcept
    {
        if (clapNext < clapBursts && age >= clapBurstAt[clapNext])
        {
            burstEnv = clapBurstAmp[clapNext];
            if (++clapNext == clapBursts)
                env = kClapTailLevel;
        }
        ++age;

        const float w = rng.bipolar();
        const float v = 3.0f * burstEnv * filter.bandpass (w) + 1.6f * env * filterR.bandpass (w);
        burstEnv *= burstMul;
        env *= envMul;
        if (clapNext == clapBursts && env < 1.0e-4f && burstEnv < 1.0e-4f)
            active = false;
        return v;
    }
};

} // namespace drum
