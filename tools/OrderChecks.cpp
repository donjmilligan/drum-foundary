// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// The basic body waveforms (Sine, Saw, Square, Round Square, Triangle), Body Style's move
// of Sample in format 7, and the preset list's Kick / Snare grouping.

#include "PluginProcessor.h"
#include "dsp/BodyGen.h"
#include "presets/FactoryPresets.h"

#include <cstdio>
#include <juce_dsp/juce_dsp.h>

namespace drum::orderchecks
{

int run();

namespace
{
    constexpr float kSr = 48000.0f;

    std::vector<float> renderBody (int style, float hz, float shape, int samples)
    {
        BodyGen gen;
        gen.start (style, shape, 0.0f, 1.0f, kSr);
        BodyGen::Settings s;
        s.gain = 1.0f;
        s.shape = shape;
        s.baseInc = hz / kSr;
        s.lengthSamples = (float) samples;
        std::vector<float> pitch ((size_t) samples, 0.0f), level ((size_t) samples, 1.0f), out ((size_t) samples), couple ((size_t) samples);
        for (int i = 0; i < samples; i += 256)
            gen.render (i, std::min (samples, i + 256), s, pitch.data(), level.data(), out.data(), couple.data());
        return out;
    }

    /** The loudest component that isn't a harmonic of `hz`, in dB under the fundamental. */
    float worstAliasDb (const std::vector<float>& x, float hz)
    {
        constexpr int order = 15, size = 1 << order;
        std::vector<float> buf ((size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[(size_t) i] * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) size));
        juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (buf.data());
        const float binHz = kSr / (float) size;
        const float fund = buf[(size_t) juce::roundToInt (hz / binHz)];
        float worst = 0.0f;
        for (int b = 2; b < size / 2; ++b)
        {
            const float f = (float) b * binHz;
            if (f > 20000.0f)
                break;
            const float h = f / hz;
            if (std::abs (h - std::round (h)) * hz < 6.0f * binHz) // a harmonic (and its window skirt)
                continue;
            worst = std::max (worst, buf[(size_t) b]);
        }
        return 20.0f * std::log10 (worst / fund + 1.0e-12f);
    }
}

int run()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: order / waves: %s\n", msg.toRawUTF8()); ++failures; };

    // --- The basic waveforms: their pitch, a start from (near) 0, and their size -------------------
    {
        juce::String report;
        for (int style : { (int) bodySine, (int) bodySaw, (int) bodySquare, (int) bodyRoundSquare, (int) bodyTriangle })
            for (float shape : { 0.0f, 0.6f })
            {
                const auto x = renderBody (style, 100.0f, shape, (int) kSr);
                int rising = 0;
                float peak = 0.0f;
                for (size_t i = 1; i < x.size(); ++i)
                {
                    rising += x[i - 1] < 0.0f && x[i] >= 0.0f;
                    peak = std::max (peak, std::abs (x[i]));
                }
                if (shape == 0.0f)
                    report << bodyStyleNames[style] << " " << rising << " Hz  ";
                if (std::abs (rising - 100) > 1 || std::abs (x[0]) > 0.05f || peak > 1.6f || peak < 0.5f)
                    fail (bodyStyleNames[style] + " (Shape " + juce::String (shape) + "): " + juce::String (rising) + " cycles in 1 s at 100 Hz, first sample "
                          + juce::String (x[0]) + ", peak " + juce::String (peak));
            }
        std::printf ("basic waveforms at 100 Hz (rising zero crossings in 1 s): %s\n", report.toRawUTF8());

        // Band-limited: a 3.3 kHz saw and square have their aliases well under the fundamental.
        for (int style : { (int) bodySaw, (int) bodySquare })
        {
            const float alias = worstAliasDb (renderBody (style, 3300.0f, 0.0f, 1 << 15), 3300.0f);
            std::printf ("  %s at 3.3 kHz: loudest alias %.1f dB under the fundamental\n", bodyStyleNames[style].toRawUTF8(), -alias);
            if (alias > -30.0f)
                fail (bodyStyleNames[style] + " at 3.3 kHz aliases at " + juce::String (alias, 1) + " dB");
        }

        // The Shapes do something.
        const auto plain = renderBody (bodySquare, 100.0f, 0.0f, 4800), narrow = renderBody (bodySquare, 100.0f, 1.0f, 4800);
        int high0 = 0, high1 = 0;
        for (size_t i = 480; i < 4800; ++i)
        {
            high0 += plain[i] > 0.0f;
            high1 += narrow[i] > 0.0f;
        }
        if (! (high1 < high0 / 2))
            fail ("Square's Width doesn't narrow the pulse");
    }

    // --- Body Style: format 3-6 Sample (11) loads as Sample; the waveforms keep theirs ---------------
    {
        const auto styleAfterLoading = [] (int format, double value)
        {
            DrumFoundryProcessor proc;
            auto tree = proc.createStateTree();
            tree.setProperty (DrumFoundryProcessor::formatVersionId, format, nullptr);
            for (auto child : tree)
                if (child.getProperty ("id").toString() == pid::bodyStyle)
                    child.setProperty ("value", value, nullptr);
            DrumFoundryProcessor loaded;
            loaded.loadStateTree (tree);
            return juce::roundToInt (loaded.getState().getRawParameterValue (pid::bodyStyle)->load());
        };
        const int oldSample = styleAfterLoading (6, 11.0), tonal = styleAfterLoading (6, 10.0), sine = styleAfterLoading (7, 11.0),
                  older = styleAfterLoading (2, 6.0);
        if (oldSample != bodySample || tonal != bodyTonal || sine != bodySine || older != bodySample)
            fail ("Body Style migration: format 6 Sample -> " + juce::String (oldSample) + ", Tonal -> " + juce::String (tonal)
                  + ", format 7 Sine -> " + juce::String (sine) + ", format 2 Sample -> " + juce::String (older));
    }

    // --- The preset list: kicks, then snares, each with its Init first, then A to Z ---------------
    {
        DrumFoundryProcessor proc;
        auto& presets = proc.getPresets();
        const auto& entries = presets.getEntries();
        const auto& order = presets.getDisplayOrder();
        if (order.size() != entries.size())
        {
            fail ("the display order doesn't list every preset once");
        }
        else
        {
            juce::StringArray names;
            for (int i : order)
                names.add (entries[(size_t) i].name);
            int firstSnare = -1;
            for (int k = 0; k < names.size() && firstSnare < 0; ++k)
                if (entries[(size_t) order[(size_t) k]].isFactory && entries[(size_t) order[(size_t) k]].category == PresetManager::Category::snare)
                    firstSnare = k;
            bool kicksFirst = firstSnare > 0, sorted = true;
            for (int k = 0; k < firstSnare; ++k)
                kicksFirst = kicksFirst && names[k].containsIgnoreCase ("kick");
            for (int k = firstSnare + 1; k < names.size(); ++k)
                if (entries[(size_t) order[(size_t) k]].isFactory && ! names[k].containsIgnoreCase ("kick") && k > firstSnare + 1)
                    sorted = sorted && names[k - 1].compareNatural (names[k]) <= 0;
            if (names[0] != "Init Kick" || ! kicksFirst || firstSnare < 0 || names[firstSnare] != "Init" || ! sorted)
                fail ("the preset list isn't Init Kick, the kicks, Init, the snares (A to Z): starts " + names[0] + ", snares start at "
                      + juce::String (firstSnare));
            std::printf ("preset list: %d kicks (Init Kick first), then %d snares (Init first), A to Z\n", firstSnare,
                         (int) factoryPresets().size() - firstSnare);

            // Next / previous follow the list as it's shown, and the entries keep their indices.
            presets.load (order[0]);
            presets.loadNext();
            if (presets.getCurrentName() != names[1] || entries[0].name != "Init")
                fail ("Next didn't go to the second preset in the list (" + presets.getCurrentName() + "), or entry 0 moved");
            presets.loadPrevious();
            presets.loadPrevious();
            if (presets.getCurrentName() != names[names.size() - 1])
                fail ("Previous from the top didn't wrap to the bottom of the list");
        }
    }

    std::printf ("order / waves: basic waveforms, band limiting, Body Style migration, preset grouping %s\n", failures == 0 ? "ok" : "FAILED");
    return failures;
}

} // namespace drum::orderchecks
