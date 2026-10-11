// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// The compressor's characters (CompModels.h): their ratios, what makes each one itself,
// staying transparent when off, and the per-compressor capture the editor draws.

#include "PluginProcessor.h"
#include "dsp/Compressor.h"
#include "dsp/OfflineRenderer.h"
#include "ui/CompScope.h"

#include <cstdio>
#include <random>

namespace drum::compchecks
{

int run();

namespace
{
    constexpr double kSr = 48000.0;
    constexpr double kPi = 3.14159265358979323846;

    CompSettings settings (int style, float threshold = -24.0f, float ratio = 4.0f, float attack = 5.0f, float release = 120.0f, float knee = 0.0f)
    {
        CompSettings s;
        s.on = true;
        s.style = style;
        s.thresholdDb = threshold;
        s.ratio = ratio;
        s.attackMs = attack;
        s.releaseMs = release;
        s.kneeDb = knee;
        s.makeupDb = 0.0f;
        s.mix = 1.0f;
        return s;
    }

    /** A sine at `levelDb` peak for `seconds`, then (with `quietDb` > -200) at that level. */
    std::vector<float> sine (double hz, float levelDb, double seconds, float quietDb = -300.0f, double quietSeconds = 0.0)
    {
        const int n1 = (int) (seconds * kSr), n2 = (int) (quietSeconds * kSr);
        std::vector<float> x ((size_t) (n1 + n2));
        const float a1 = juce::Decibels::decibelsToGain (levelDb), a2 = quietDb > -200.0f ? juce::Decibels::decibelsToGain (quietDb) : 0.0f;
        for (int i = 0; i < n1 + n2; ++i)
            x[(size_t) i] = (i < n1 ? a1 : a2) * (float) std::sin (2.0 * kPi * hz * i / kSr);
        return x;
    }

    /** Runs `x` through a compressor (as stereo), returning the output and the gain change per sample. */
    std::pair<std::vector<float>, std::vector<float>> compress (const std::vector<float>& x, const CompSettings& s)
    {
        Compressor c;
        c.prepare (kSr);
        juce::AudioBuffer<float> cap (3, (int) x.size());
        c.setCapture (&cap);
        std::vector<float> l = x, r = x;
        for (size_t i = 0; i < x.size(); i += 256)
        {
            const int n = (int) std::min<size_t> (256, x.size() - i);
            c.process (l.data() + i, r.data() + i, n, s);
        }
        std::vector<float> gr ((size_t) cap.getNumSamples());
        for (int i = 0; i < cap.getNumSamples(); ++i)
            gr[(size_t) i] = cap.getSample (2, i);
        return { l, gr };
    }

    double rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return 10.0 * std::log10 (sum / (double) std::max<size_t> (1, to - from) + 1.0e-30);
    }

    /** The level (dB, relative to the fundamental) of harmonic `h` of `hz` in x[from..to). */
    double harmonicDb (const std::vector<float>& x, size_t from, size_t to, double hz, int h)
    {
        const auto bin = [&] (double f)
        {
            double re = 0.0, im = 0.0;
            for (size_t i = from; i < to; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) (i - from) / (double) (to - from));
                re += w * x[i] * std::cos (2.0 * kPi * f * (double) i / kSr);
                im += w * x[i] * std::sin (2.0 * kPi * f * (double) i / kSr);
            }
            return std::hypot (re, im);
        };
        return 20.0 * std::log10 (bin (hz * h) / std::max (1.0e-20, bin (hz)));
    }

    /** Time (ms) after `from` until the gain change is back within `withinDb` of 0. */
    double recoveryMs (const std::vector<float>& gr, size_t from, float withinDb)
    {
        for (size_t i = from; i < gr.size(); ++i)
            if (gr[i] > -withinDb)
                return (double) (i - from) / kSr * 1000.0;
        return 1.0e9;
    }
}

int run()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: comp: %s\n", msg.toRawUTF8()); ++failures; };

    // --- Every character's ratio is the knob's: 12 dB over a -24 dB threshold at 4:1 is 9 dB of
    //     reduction. A square wave, so peak and RMS read the same level ----------------------------
    {
        juce::String report;
        auto x = sine (100.0, -12.0f, 1.0);
        for (auto& v : x)
            v = v >= 0.0f ? juce::Decibels::decibelsToGain (-12.0f) : -juce::Decibels::decibelsToGain (-12.0f);
        for (int style = 0; style < numCompStyles; ++style)
        {
            const auto [y, gr] = compress (x, settings (style));
            const double measured = rmsDb (y, x.size() / 2, x.size()) - rmsDb (x, x.size() / 2, x.size());
            const double expected = -9.0;
            report << compStyleNames[style] << " " << juce::String (measured, 2) << "  ";
            if (std::abs (measured - expected) > 0.5)
                fail (compStyleNames[style] + ": " + juce::String (measured, 2) + " dB of reduction at 12 dB over, 4:1 (expected "
                      + juce::String (expected, 2) + ")");
        }
        std::printf ("comp ratios (a square wave 12 dB over at 4:1, expect -9 dB): %s\n", report.toRawUTF8());
    }

    // --- What makes each one itself ---------------------------------------------------------------
    {
        // FET: odd harmonics that grow as it digs in; Clean adds none worth the name.
        const auto x = sine (500.0, -6.0f, 0.6);
        const auto fet = compress (x, settings (compFet, -30.0f, 8.0f, 1.0f, 200.0f)).first;
        const auto clean = compress (x, settings (compClean, -30.0f, 8.0f, 1.0f, 200.0f)).first;
        const double fetH3 = harmonicDb (fet, x.size() / 2, x.size(), 500.0, 3);
        const double cleanH3 = harmonicDb (clean, x.size() / 2, x.size(), 500.0, 3);
        std::printf ("comp harmonics under ~20 dB of reduction: FET 3rd %.1f dB (Clean %.1f)\n", fetH3, cleanH3);
        if (! (fetH3 > cleanH3 + 10.0 && fetH3 > -55.0 && fetH3 < -26.0))
            fail ("FET's 3rd harmonic under heavy reduction is " + juce::String (fetH3, 1) + " dB (Clean " + juce::String (cleanH3, 1)
                  + "): expected a clear but modest rise");

        // Opto: even harmonics (its tube stage), modest.
        const auto opto = compress (x, settings (compOpto, -30.0f, 4.0f, 10.0f, 200.0f)).first;
        const double optoH2 = harmonicDb (opto, x.size() / 2, x.size(), 500.0, 2);
        if (! (optoH2 > -60.0 && optoH2 < -25.0))
            fail ("Opto's 2nd harmonic is " + juce::String (optoH2, 1) + " dB: expected a little warmth");

        // Opto remembers: after 2 s of heavy reduction it lets go slower than after a 60 ms burst.
        // (An LA-2A takes seconds to let go fully after long, heavy compression: so does this.)
        const auto longHit = compress (sine (200.0, -6.0f, 2.0, -80.0f, 8.0), settings (compOpto, -30.0f, 4.0f, 10.0f, 100.0f)).second;
        const auto shortHit = compress (sine (200.0, -6.0f, 0.06, -80.0f, 8.0), settings (compOpto, -30.0f, 4.0f, 10.0f, 100.0f)).second;
        const double longRec = recoveryMs (longHit, (size_t) (2.0 * kSr), 1.0f), shortRec = recoveryMs (shortHit, (size_t) (0.06 * kSr), 1.0f);
        std::printf ("comp Opto release to within 1 dB: %.0f ms after 2 s of reduction, %.0f ms after a 60 ms burst\n", longRec, shortRec);
        if (longRec > 7000.0)
            fail ("Opto never lets go after long compression (" + juce::String (longRec, 0) + " ms)");
        if (! (longRec > shortRec * 1.5))
            fail ("Opto's release doesn't slow after long compression (" + juce::String (longRec, 0) + " vs " + juce::String (shortRec, 0) + " ms)");

        // Bus: auto release, quick after a short burst, slow after sustained level.
        const auto busLong = compress (sine (200.0, -6.0f, 1.5, -80.0f, 2.0), settings (compBus, -30.0f, 4.0f, 3.0f, 200.0f)).second;
        const auto busShort = compress (sine (200.0, -6.0f, 0.02, -80.0f, 2.0), settings (compBus, -30.0f, 4.0f, 3.0f, 200.0f)).second;
        const double busLongRec = recoveryMs (busLong, (size_t) (1.5 * kSr), 1.0f), busShortRec = recoveryMs (busShort, (size_t) (0.02 * kSr), 1.0f);
        std::printf ("comp Bus auto release to within 1 dB: %.0f ms after 1.5 s, %.0f ms after a 20 ms burst\n", busLongRec, busShortRec);
        if (! (busLongRec > busShortRec * 2.0))
            fail ("Bus's auto release doesn't follow the program (" + juce::String (busLongRec, 0) + " vs " + juce::String (busShortRec, 0) + " ms)");

        // Punch: a release at a constant rate (120 dB/s at Release 120 ms): 6 dB per 50 ms.
        const auto punch = compress (sine (200.0, -6.0f, 0.5, -300.0f, 1.0), settings (compPunch, -36.0f, 10.0f, 5.0f, 120.0f)).second;
        const size_t off = (size_t) (0.5 * kSr);
        const float g0 = punch[off + (size_t) (0.02 * kSr)], g1 = punch[off + (size_t) (0.07 * kSr)], g2 = punch[off + (size_t) (0.12 * kSr)];
        if (! (std::abs ((g1 - g0) - 6.0f) < 0.6f && std::abs ((g2 - g1) - 6.0f) < 0.6f))
            fail ("Punch's release isn't 120 dB/s: " + juce::String (g1 - g0, 2) + " then " + juce::String (g2 - g1, 2) + " dB per 50 ms");

        // Punch: attack speeds up the harder it's hit (dbx-style).
        const auto attackTime = [&] (float levelDb)
        {
            const auto gr = compress (sine (200.0, levelDb, 0.3), settings (compPunch, -40.0f, 10.0f, 15.0f, 120.0f)).second;
            const float final = gr.back();
            for (size_t i = 0; i < gr.size(); ++i)
                if (gr[i] < final * 0.9f)
                    return (double) i / kSr * 1000.0;
            return 1.0e9;
        };
        const double softAttack = attackTime (-28.0f), hardAttack = attackTime (-6.0f);
        if (! (hardAttack < softAttack * 0.7))
            fail ("Punch's attack doesn't speed up when hit harder (" + juce::String (hardAttack, 1) + " vs " + juce::String (softAttack, 1) + " ms)");
    }

    // --- Off is bit-transparent in every style; random settings stay finite ---------------------------
    {
        std::mt19937 rng (11);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        bool transparent = true, finite = true;
        float peak = 0.0f;
        for (int style = 0; style < numCompStyles; ++style)
        {
            auto s = settings (style);
            s.on = false;
            const auto x = sine (300.0, -3.0f, 0.2);
            transparent = transparent && compress (x, s).first == x;

            for (int trial = 0; trial < 6; ++trial)
            {
                auto r = settings (style, -48.0f * u (rng), 1.0f + 19.0f * u (rng), 0.1f + 100.0f * u (rng) * u (rng),
                                   10.0f + 990.0f * u (rng), 12.0f * u (rng));
                r.makeupDb = 12.0f * u (rng);
                r.mix = u (rng);
                std::vector<float> noise ((size_t) kSr / 2);
                for (auto& v : noise)
                    v = (u (rng) * 2.0f - 1.0f) * (u (rng) > 0.98f ? 4.0f : 0.5f);
                for (float v : compress (noise, r).first)
                {
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, std::abs (v));
                }
            }
        }
        if (! transparent)
            fail ("a compressor switched off changes the signal");
        if (! finite || peak > 100.0f)
            fail ("random compressor settings: " + juce::String (finite ? "finite" : "NOT finite") + ", peak " + juce::String (peak));
    }

    // --- The capture: aligned with the rendered hit, and what the scope draws ----------------------
    {
        DrumFoundryProcessor proc;
        auto p = proc.readParams();
        p.chain = FxChain {};
        p.chain.add (fxComp1);
        p.comp[0] = settings (compPunch, -30.0f, 6.0f, 2.0f, 80.0f);
        p.clipMode = 0;          // Off: the output stage only delays
        p.revMix = 0.0f;
        EnvTables tables;
        proc.bakeTables (tables);
        CompCaptures caps;
        const auto hit = renderHit (p, tables, kSr, 60, 1.0f, 0x5EEDu, {}, nullptr, &caps);
        const auto& c = caps.comp[0];
        if (c.getNumSamples() != hit.getNumSamples() || caps.comp[1].getNumSamples() != 0 || caps.comp[2].getNumSamples() != 0)
        {
            fail ("the capture isn't the hit's length (or a compressor that isn't working has one)");
        }
        else
        {
            // Comp 1 is the whole chain, so its output (mono) is the hit (mono), sample for sample.
            float worst = 0.0f, minGr = 0.0f;
            for (int i = 0; i < hit.getNumSamples(); ++i)
            {
                worst = std::max (worst, std::abs (c.getSample (1, i) - 0.5f * (hit.getSample (0, i) + hit.getSample (1, i))));
                minGr = std::min (minGr, c.getSample (2, i));
            }
            std::printf ("comp capture: aligned with the hit within %.2e, most reduction %.1f dB\n", worst, minGr);
            if (worst > 1.0e-4f || ! (minGr < -3.0f))
                fail ("the capture isn't aligned with the hit (" + juce::String (worst) + ") or shows no reduction (" + juce::String (minGr) + " dB)");

            ui::CompScope scope (juce::Colours::orange);
            scope.setSize (400, 90);
            scope.setCapture (c, kSr);
            if (! scope.hasCapture() || std::abs (scope.getMaxReductionDb() - minGr) > 0.01f)
                fail ("the scope doesn't show the capture's reduction");
        }
    }

    std::printf ("comp: ratios, FET / Opto harmonics, Opto memory, Bus auto release, Punch rate and attack, off, random, capture %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

} // namespace drum::compchecks
