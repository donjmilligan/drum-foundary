// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// The EQ (ParametricEq) and its full-screen editor: filter shapes against the analog
// curves, the Digital style against the old 3-band EQ, slopes, dynamics, Listen, old
// patches, and the editor's graph.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "dsp/DspUtils.h"
#include "dsp/ParametricEq.h"
#include "ui/EqPanel.h"

#include <cstdio>
#include <random>

namespace drum::eqchecks
{

int run();
int runEditor();

namespace
{
    juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods = {},
                              int clicks = 1, juce::Point<float> downPos = { -1.0f, -1.0f }, bool dragged = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                 juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                 juce::MouseInputSource::defaultTiltY, &c, &c, now,
                                 downPos.x < 0.0f ? pos : downPos, now, clicks, dragged);
    }

    template <typename T>
    T* findChild (juce::Component& root)
    {
        for (auto* c : root.getChildren())
        {
            if (auto* t = dynamic_cast<T*> (c))
                return t;
            if (auto* t = findChild<T> (*c))
                return t;
        }
        return nullptr;
    }

    float raw (DrumFoundryProcessor& p, const juce::String& id)
    {
        const auto* v = p.getState().getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    }

    double db (double linear) { return 20.0 * std::log10 (std::max (1.0e-12, linear)); }

    DrumParams::EqBand makeBand (int type, float freq, float gainDb, float q, int slope = 3)
    {
        DrumParams::EqBand b;
        b.on = true;
        b.type = type;
        b.freq = freq;
        b.gainDb = gainDb;
        b.q = q;
        b.slope = slope;
        return b;
    }

    /** Largest |digital - analog| in dB from 20 Hz up to `topFraction` of Nyquist, where the
        analog curve is above -60 dB. */
    double worstError (const DrumParams::EqBand& b, int style, double sr, double topFraction)
    {
        const auto d = ParametricEq::design (b, style, sr);
        double worst = 0.0;
        const double top = 0.5 * sr * topFraction;
        for (int i = 0; i <= 300; ++i)
        {
            const double hz = 20.0 * std::pow (top / 20.0, i / 300.0);
            const double analog = ParametricEq::analogMagnitudeDb (b, style, hz);
            // Where an EQ curve matters: deep in a cut's stopband or a notch, a few dB
            // either way is inaudible.
            if (analog < -24.0)
                continue;
            worst = std::max (worst, std::abs (db (d.magnitude (hz, sr)) - analog));
        }
        return worst;
    }

    /** Steady-state gain (dB) of a sine at `hz` through the EQ. */
    double sineGainDb (DrumParams p, double hz, double sr, float level = 0.25f, int seconds10 = 6)
    {
        ParametricEq eq;
        eq.prepare (sr);
        const int n = (int) (sr * seconds10 / 10.0);
        std::vector<float> l ((size_t) n), r ((size_t) n);
        for (int i = 0; i < n; ++i)
            l[(size_t) i] = r[(size_t) i] = level * (float) std::sin (2.0 * 3.14159265358979 * hz * i / sr);
        for (int i = 0; i < n; i += 512)
            eq.process (l.data() + i, r.data() + i, std::min (512, n - i), p, p.eqOn);
        double in = 0.0, out = 0.0;
        for (int i = n / 2; i < n; ++i)
        {
            const double x = level * std::sin (2.0 * 3.14159265358979 * hz * i / sr);
            in += x * x;
            out += (double) l[(size_t) i] * l[(size_t) i];
        }
        return 10.0 * std::log10 (out / in);
    }

    DrumParams flatEq()
    {
        DrumParams p;
        for (auto& b : p.eqBands)
            b.on = false;
        return p;
    }
}

int run()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: eq: %s\n", msg.toRawUTF8()); ++failures; };

    // --- Shapes against the analog curves (20 Hz to 95% of Nyquist, where the curve is above
    //     -24 dB), bands up to 12 kHz at 44.1 / 48 / 96 kHz. Clean must be no worse than
    //     Digital, and well inside these bounds (bells, the common case, within 1 dB). ---------
    {
        // Worst seen when this was written, plus a little: Clean / Digital.
        const double bound[] { 1.0, 2.2, 7.0, 3.6, 2.3, 6.6, 4.3, 2.0 };
        juce::String report;
        for (int type = 0; type < numEqTypes; ++type)
        {
            double worstC = 0.0, worstD = 0.0;
            juce::String where;
            const bool shelf = type == eqLowShelf || type == eqHighShelf;
            for (double sr : { 44100.0, 48000.0, 96000.0 })
                for (float freq : { 40.0f, 200.0f, 1000.0f, 5000.0f, 8000.0f, 12000.0f })
                    for (float gain : { -18.0f, -6.0f, 6.0f, 18.0f })
                        for (float q : { 0.5f, 1.0f, 2.0f, 4.0f })
                        {
                            if (shelf && q > 2.0f)
                                continue;
                            const auto b = makeBand (type, freq, gain, q);
                            const double c = worstError (b, eqStyleClean, sr, 0.95);
                            const double d = worstError (b, eqStyleDigital, sr, 0.95);
                            worstD = std::max (worstD, d);
                            if (c > worstC)
                            {
                                worstC = c;
                                where = juce::String (freq) + " Hz " + juce::String (gain) + " dB Q " + juce::String (q) + " at " + juce::String (sr);
                            }
                            if (c > d + 0.05)
                                fail (eqTypeNames[type] + " " + where + ": Clean is further from the analog curve than Digital");
                        }
            report << eqTypeNames[type] << " " << juce::String (worstC, 2) << " (Digital " << juce::String (worstD, 1) << ")  ";
            if (worstC > bound[type])
                fail (eqTypeNames[type] + ": Clean is " + juce::String (worstC, 2) + " dB off the analog curve (" + where + ")");
        }
        std::printf ("eq shapes, worst dB off the analog curve: %s\n", report.toRawUTF8());
    }

    // --- Digital is the old 3-band EQ: bands 1-3 at their defaults ---------------------------
    {
        double worst = 0.0;
        for (float sr : { 44100.0f, 48000.0f, 96000.0f })
            for (float g : { -15.0f, -4.0f, 3.0f, 15.0f })
            {
                DrumParams p;
                dsp::Biquad oldLow, oldMid, oldHigh;
                oldLow.setLowShelf (sr, 100.0f, g);
                oldMid.setPeak (sr, 2500.0f, 0.9f, -g);
                oldHigh.setHighShelf (sr, 7000.0f, g * 0.5f);
                auto mid = p.eqBands[1];
                mid.freq = 2500.0f;
                const std::pair<EqDesign, const dsp::Biquad*> pairs[] {
                    { ParametricEq::design (p.eqBands[0], eqStyleDigital, sr, g), &oldLow },
                    { ParametricEq::design (mid, eqStyleDigital, sr, -g), &oldMid },
                    { ParametricEq::design (p.eqBands[2], eqStyleDigital, sr, g * 0.5f), &oldHigh } };
                for (const auto& [d, old] : pairs)
                {
                    if (d.count != 1)
                    {
                        fail ("a Digital band 1-3 isn't one section");
                        continue;
                    }
                    const auto& sec = d.sections[0];
                    for (double diff : { sec.b0 - old->b0, sec.b1 - old->b1, sec.b2 - old->b2, sec.a1 - old->a1, sec.a2 - old->a2 })
                        worst = std::max (worst, std::abs (diff));
                }
            }
        std::printf ("eq Digital against the old 3-band EQ's biquads: largest coefficient difference %.2e\n", worst);
        if (worst > 2.0e-5)
            fail ("Digital bands 1-3 aren't the old 3-band EQ (coefficients differ by " + juce::String (worst) + ")");
    }

    // --- Cuts: -3 dB at the corner (Q 0.71), and the slope two octaves out ---------------------
    {
        juce::String report;
        for (int slope = 0; slope < eqSlopeNames.size(); ++slope)
            for (int type : { (int) eqLowCut, (int) eqHighCut })
            {
                const auto b = makeBand (type, type == eqLowCut ? 1000.0f : 2000.0f, 0.0f, 0.70710678f, slope);
                const auto d = ParametricEq::design (b, eqStyleClean, 48000.0);
                const double corner = db (d.magnitude (b.freq, 48000.0));
                const double out = db (d.magnitude (type == eqLowCut ? b.freq / 4.0 : b.freq * 4.0, 48000.0));
                // Butterworth two octaves out: -10 log10 (1 + 4^(2N)).
                const double expected = -10.0 * std::log10 (1.0 + std::pow (4.0, 2.0 * eqSlopeOrder (slope)));
                if (type == eqLowCut)
                    report << eqSlopeNames[slope] << " " << juce::String (out, 1) << "  ";
                if (std::abs (corner + 3.01) > 0.15 || std::abs (out - expected) > 1.0)
                    fail (eqTypeNames[type] + " " + eqSlopeNames[slope] + ": " + juce::String (corner, 2) + " dB at the corner (expected -3.01), "
                          + juce::String (out, 1) + " dB two octaves out (expected " + juce::String (expected, 0) + ")");
            }
        std::printf ("eq low cut at 1 kHz, level at 250 Hz per slope: %s\n", report.toRawUTF8());

        // Q above 0.71 lifts the corner.
        const auto res = ParametricEq::design (makeBand (eqLowCut, 1000.0f, 0.0f, 4.0f, 3), eqStyleClean, 48000.0);
        if (! (db (res.magnitude (1000.0, 48000.0)) > 6.0))
            fail ("a resonant low cut (Q 4) has no peak at its corner");
    }

    // --- Processing: a sine comes out at the design's gain, in every style ----------------------
    {
        double worst = 0.0;
        for (int style = 0; style < numEqStyles; ++style)
        {
            auto p = flatEq();
            p.eqStyle = style;
            p.eqBands[0] = makeBand (eqBell, 800.0f, 9.0f, 2.0f);
            p.eqBands[3] = makeBand (eqHighShelf, 6000.0f, -6.0f, 0.7071f);
            p.eqBands[5] = makeBand (eqLowCut, 60.0f, 0.0f, 0.7071f, 4);
            for (double hz : { 50.0, 400.0, 800.0, 3000.0, 12000.0 })
            {
                double expected = 0.0;
                for (const auto& b : p.eqBands)
                    if (b.on)
                        expected += db (ParametricEq::design (b, style, 48000.0).magnitude (hz, 48000.0));
                worst = std::max (worst, std::abs (sineGainDb (p, hz, 48000.0) - expected));
            }
        }
        std::printf ("eq processing against the designed curve (sines, 4 styles): within %.3f dB\n", worst);
        if (worst > 0.05)
            fail ("a sine through the EQ is " + juce::String (worst, 3) + " dB off the designed curve");

        // The styles differ where they should: British narrows a boosted bell, Pultec's shelf dips.
        auto bell = makeBand (eqBell, 1000.0f, 12.0f, 1.0f);
        const double clean = db (ParametricEq::design (bell, eqStyleClean, 48000.0).magnitude (1500.0, 48000.0));
        const double british = db (ParametricEq::design (bell, eqStyleBritish, 48000.0).magnitude (1500.0, 48000.0));
        auto shelf = makeBand (eqLowShelf, 100.0f, 9.0f, 0.7071f);
        const double pultecDip = db (ParametricEq::design (shelf, eqStylePultec, 48000.0).magnitude (300.0, 48000.0));
        const double cleanDip = db (ParametricEq::design (shelf, eqStyleClean, 48000.0).magnitude (300.0, 48000.0));
        if (! (british < clean - 1.0) || ! (pultecDip < -0.5 && cleanDip > -0.1))
            fail ("the styles don't shape the curve: British " + juce::String (british, 2) + " vs Clean " + juce::String (clean, 2)
                  + " dB half an octave off a +12 dB bell; Pultec shelf " + juce::String (pultecDip, 2) + " vs Clean " + juce::String (cleanDip, 2) + " dB above it");
    }

    // --- Dynamic: a loud band is cut by the band's Gain, a quiet one isn't ---------------------
    {
        auto p = flatEq();
        p.eqBands[0] = makeBand (eqBell, 100.0f, -12.0f, 1.0f);
        p.eqBands[0].dynamic = true;
        p.eqBands[0].thresholdDb = -30.0f;
        const double loud = sineGainDb (p, 100.0, 48000.0, 0.5f, 10);
        const double quiet = sineGainDb (p, 100.0, 48000.0, 0.003f, 10);
        std::printf ("eq dynamic bell (-12 dB at 100 Hz, threshold -30 dB): %.2f dB on a -6 dBFS sine, %.2f dB on a -50 dBFS one\n", loud, quiet);
        if (std::abs (loud + 12.0) > 0.5 || std::abs (quiet) > 0.2)
            fail ("the dynamic band doesn't cut only when loud: " + juce::String (loud, 2) + " / " + juce::String (quiet, 2) + " dB");

        // The applied gain is reported for the UI.
        ParametricEq eq;
        eq.prepare (48000.0);
        std::vector<float> l (4800), r (4800);
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = r[i] = 0.5f * (float) std::sin (2.0 * 3.14159265358979 * 100.0 * (double) i / 48000.0);
        eq.process (l.data(), r.data(), (int) l.size(), p, true);
        if (std::abs (eq.getAppliedGainDb (0) + 12.0f) > 0.5f)
            fail ("the dynamic band reports " + juce::String (eq.getAppliedGainDb (0), 2) + " dB applied (expected -12)");
    }

    // --- Listen: a band pass of the input -----------------------------------------------------
    {
        auto p = flatEq();
        p.eqBands[0] = makeBand (eqBell, 1000.0f, -18.0f, 1.0f); // ignored while listening
        p.eqListenHz = 1000.0f;
        p.eqListenQ = 4.0f;
        const double at = sineGainDb (p, 1000.0, 48000.0), off = sineGainDb (p, 100.0, 48000.0);
        if (std::abs (at) > 0.2 || off > -20.0)
            fail ("Listen at 1 kHz: " + juce::String (at, 2) + " dB at 1 kHz (expected 0), " + juce::String (off, 1) + " dB at 100 Hz");
    }

    // --- Off is flat; random settings stay finite ------------------------------------------------
    {
        auto p = DrumParams();
        for (auto& b : p.eqBands)
            b = makeBand (eqBell, 500.0f, 12.0f, 2.0f);
        p.eqOn = false;
        if (std::abs (sineGainDb (p, 500.0, 48000.0)) > 1.0e-4)
            fail ("the EQ switched off isn't flat");

        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        float peak = 0.0f;
        bool finite = true;
        for (int trial = 0; trial < 40; ++trial)
        {
            DrumParams q;
            q.eqStyle = trial % numEqStyles;
            for (auto& b : q.eqBands)
            {
                b = makeBand ((int) (u (rng) * (float) numEqTypes) % numEqTypes, 20.0f * std::pow (1000.0f, u (rng)), -24.0f + 48.0f * u (rng),
                              0.1f * std::pow (180.0f, u (rng)), (int) (u (rng) * 8) % 8);
                b.on = u (rng) > 0.3f;
                b.dynamic = u (rng) > 0.6f;
                b.thresholdDb = -40.0f * u (rng);
            }
            ParametricEq eq;
            eq.prepare (44100.0);
            std::vector<float> l (8192), r (8192);
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = r[i] = 0.25f * (u (rng) * 2.0f - 1.0f);
            for (int i = 0; i < 8192; i += 256)
            {
                // Settings move between blocks, as automation would move them.
                q.eqBands[(size_t) (i / 256) % kNumEqBands].freq *= 1.05f;
                eq.process (l.data() + i, r.data() + i, 256, q, true);
            }
            for (float v : l)
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
        }
        if (! finite || peak > 400.0f)
            fail ("random EQ settings: " + juce::String (finite ? "finite" : "NOT finite") + ", peak " + juce::String (peak));
    }

    // --- Old patches load on Digital (their old curves); new ones on Clean ------------------------
    {
        DrumFoundryProcessor proc;
        if (juce::roundToInt (proc.getState().getRawParameterValue (pid::eqStyle)->load()) != eqStyleClean)
            fail ("a new patch's EQ Style isn't Clean");
        auto tree = proc.createStateTree();
        tree.setProperty (DrumFoundryProcessor::formatVersionId, 4, nullptr);
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (tree.getChild (i).getProperty ("id").toString() == pid::eqStyle)
                tree.removeChild (i, nullptr);
        DrumFoundryProcessor loaded;
        loaded.loadStateTree (tree);
        if (juce::roundToInt (loaded.getState().getRawParameterValue (pid::eqStyle)->load()) != eqStyleDigital)
            fail ("a format 4 patch's EQ didn't load on the Digital style");
        const auto p = loaded.readParams();
        if (! (p.eqBands[0].on && p.eqBands[0].type == eqLowShelf && juce::approximatelyEqual (p.eqBands[0].freq, 100.0f)
               && p.eqBands[1].on && p.eqBands[1].type == eqBell && juce::approximatelyEqual (p.eqBands[1].q, 0.9f)
               && p.eqBands[2].on && p.eqBands[2].type == eqHighShelf && juce::approximatelyEqual (p.eqBands[2].freq, 7000.0f)
               && ! p.eqBands[3].on))
            fail ("an old patch's bands 1-3 aren't the old 3-band EQ (or band 4 is on)");
        loaded.getState().getParameter (pid::eqStyle)->setValueNotifyingHost (1.0f);
        DrumFoundryProcessor again;
        again.loadStateTree (loaded.createStateTree());
        if (juce::roundToInt (again.getState().getRawParameterValue (pid::eqStyle)->load()) != eqStyleNames.size() - 1)
            fail ("a format 5 patch's EQ Style didn't survive a round trip");
    }

    std::printf ("eq: shapes, Digital = old EQ, slopes, processing, styles, dynamic, listen, off, random settings, old patches %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures + runEditor();
}

/** The full-screen EQ: opening it, the graph's gestures, Find Peaks, Listen, Esc. */
int runEditor()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: eq editor: %s\n", msg.toRawUTF8()); ++failures; };
    const auto near = [] (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; };

    // --- findResonances on a made-up spectrum: a 6 dB bump at 2 kHz on a slope ------------------
    {
        std::vector<float> hz, level;
        for (int i = 0; i <= 48 * 10; ++i)
        {
            const float f = 20.0f * std::exp2 ((float) i / 48.0f);
            hz.push_back (f);
            const float oct = std::log2 (f / 2000.0f);
            level.push_back (-3.0f * std::log2 (f / 20.0f) + 9.0f * std::exp (-oct * oct / (2.0f * 0.03f * 0.03f)));
        }
        const auto found = ui::findResonances (hz, level);
        if (found.empty() || std::abs (std::log2 (found[0].hz / 2000.0f)) > 0.05f || found[0].prominenceDb < 4.0f)
            fail ("findResonances missed a 9 dB peak at 2 kHz (" + juce::String (found.empty() ? 0.0f : found[0].hz) + " Hz)");
        if (found.size() > 1)
            fail ("findResonances found " + juce::String ((int) found.size()) + " peaks on a smooth slope with one bump");
    }

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
    auto* overlay = findChild<ui::EqOverlay> (*editor);
    auto* graph = overlay != nullptr ? &overlay->getGraph() : nullptr;
    if (ed == nullptr || overlay == nullptr || graph == nullptr)
    {
        fail ("no editor / EQ overlay");
        return failures;
    }
    if (ed->isEqVisible())
        fail ("the EQ overlay is open in a fresh editor");

    // The EQ section's FULL button opens it.
    {
        std::function<juce::Button* (juce::Component&)> findFull = [&] (juce::Component& c) -> juce::Button*
        {
            for (auto* child : c.getChildren())
            {
                if (auto* b = dynamic_cast<juce::TextButton*> (child); b != nullptr && b->getButtonText() == "FULL")
                    return b;
                if (auto* b = findFull (*child))
                    return b;
            }
            return nullptr;
        };
        auto* full = findFull (*editor);
        if (full == nullptr)
            fail ("no FULL button on the EQ section");
        else
            full->onClick();
        if (! ed->isEqVisible())
            fail ("FULL didn't open the EQ");
    }
    ed->setEqVisible (true);
    if (! ed->isEqVisible() || overlay->getBounds() != editor->getLocalBounds().withPosition (overlay->getPosition()) || graph->getWidth() < 400)
        fail ("FULL didn't open the EQ over the whole editor");

    auto& history = proc.getUndoHistory();
    const auto drag = [] (juce::Component& c, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
    {
        c.mouseDown (mouseAt (c, from, mods));
        for (int i = 1; i <= 6; ++i)
            c.mouseDrag (mouseAt (c, from + (to - from) * ((float) i / 6.0f), mods, 1, from, true));
        c.mouseUp (mouseAt (c, to, mods, 1, from, true));
    };

    // --- Dragging band 2's node: right and up raise its Freq and Gain (the old Mid knobs) --------
    {
        const auto node = graph->getNodePosition (1);
        const float targetHz = 2000.0f, targetDb = 6.0f;
        history.clear();
        history.noteUserActivity();
        drag (*graph, node, { graph->freqToX (targetHz), graph->dbToY (targetDb) });
        history.poll (false);
        if (! near (raw (proc, pid::eqMidFreq), targetHz, 60.0f) || ! near (raw (proc, pid::eqMidGain), targetDb, 0.3f))
            fail ("dragging band 2 to 2 kHz / +6 dB left it at " + juce::String (raw (proc, pid::eqMidFreq)) + " Hz / "
                  + juce::String (raw (proc, pid::eqMidGain)) + " dB");
        if (! history.canUndo())
            fail ("the node drag isn't an undo step");
        if (graph->getSelectedBand() != 1)
            fail ("dragging band 2 didn't select it");
        if (! near (graph->getCurveDb (2000.0f), 6.0f, 0.5f))
            fail ("the graph's curve at 2 kHz isn't the band's +6 dB: " + juce::String (graph->getCurveDb (2000.0f)));
    }

    // --- Scroll on a node: its Q ---------------------------------------------------------------
    {
        const float q0 = raw (proc, eqBandParamId (1, pid::eqBandQ));
        juce::MouseWheelDetails wheel {};
        wheel.deltaY = 0.25f;
        graph->mouseWheelMove (mouseAt (*graph, graph->getNodePosition (1)), wheel);
        if (! (raw (proc, eqBandParamId (1, pid::eqBandQ)) > q0 * 1.2f))
            fail ("scrolling up on band 2 didn't raise its Q");
    }

    // --- Double-click empty space: a new bell there; on a node: that band off -------------------
    {
        const juce::Point<float> at (graph->freqToX (400.0f), graph->dbToY (-5.0f));
        graph->mouseDoubleClick (mouseAt (*graph, at, {}, 2));
        const auto b4 = [&] (const char* suffix) { return raw (proc, eqBandParamId (3, suffix)); };
        if (b4 (pid::eqBandOn) < 0.5f || juce::roundToInt (b4 (pid::eqBandType)) != eqBell || ! near (b4 (pid::eqBandFreq), 400.0f, 15.0f)
            || ! near (b4 (pid::eqBandGain), -5.0f, 0.3f) || graph->getSelectedBand() != 3)
            fail ("double-clicking at 400 Hz / -5 dB didn't add band 4 there as a bell");

        const juce::Point<float> low (graph->freqToX (24.0f), graph->dbToY (0.0f));
        graph->mouseDoubleClick (mouseAt (*graph, low, {}, 2));
        if (raw (proc, eqBandParamId (4, pid::eqBandOn)) < 0.5f || juce::roundToInt (raw (proc, eqBandParamId (4, pid::eqBandType))) != eqLowCut)
            fail ("double-clicking at 24 Hz didn't add a low cut");

        graph->mouseDoubleClick (mouseAt (*graph, graph->getNodePosition (3), {}, 2));
        if (raw (proc, eqBandParamId (3, pid::eqBandOn)) > 0.5f)
            fail ("double-clicking band 4's node didn't switch it off");
    }

    // --- Alt-drag on empty space: Listen sweeps, and stops on release ----------------------------
    {
        const juce::Point<float> from (graph->freqToX (300.0f), graph->dbToY (-15.0f)), to (graph->freqToX (3000.0f), graph->dbToY (-15.0f));
        const auto alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);
        graph->mouseDown (mouseAt (*graph, from, alt));
        graph->mouseDrag (mouseAt (*graph, to, alt, 1, from, true));
        const float listening = proc.getEqListenHz();
        graph->mouseUp (mouseAt (*graph, to, alt, 1, from, true));
        if (! near (listening, 3000.0f, 150.0f) || proc.getEqListenHz() != 0.0f)
            fail ("Alt-drag didn't sweep Listen to 3 kHz and stop it on release (" + juce::String (listening) + " Hz, then "
                  + juce::String (proc.getEqListenHz()) + ")");

        overlay->getBandEditor (1).getListenButton().setState (juce::Button::buttonDown);
        const float held = proc.getEqListenHz();
        overlay->getBandEditor (1).getListenButton().setState (juce::Button::buttonNormal);
        if (! near (held, raw (proc, pid::eqMidFreq), 1.0f) || proc.getEqListenHz() != 0.0f)
            fail ("holding band 2's LISTEN didn't listen at its frequency (and stop)");
    }

    // --- Find Peaks: a ringing 3 kHz resonance in the hit ----------------------------------------
    {
        juce::AudioBuffer<float> hit (2, 24000);
        juce::Random rng (3);
        for (int i = 0; i < hit.getNumSamples(); ++i)
        {
            const float t = (float) i / 48000.0f;
            const float v = 0.3f * std::exp (-t * 40.0f) * (rng.nextFloat() * 2.0f - 1.0f)
                          + 0.5f * std::exp (-t * 12.0f) * std::sin (2.0f * 3.14159265f * 3000.0f * t);
            hit.setSample (0, i, v);
            hit.setSample (1, i, v);
        }
        overlay->setHitAudio (hit, 48000.0);
        overlay->getFindPeaksButton().onClick();
        const auto& peaks = graph->getPeaks();
        if (peaks.empty() || std::abs (std::log2 (peaks[0].hz / 3000.0f)) > 0.05f)
        {
            fail ("Find Peaks didn't mark the 3 kHz resonance first (" + juce::String (peaks.empty() ? 0.0f : peaks[0].hz) + " Hz)");
        }
        else
        {
            const int band = graph->fixPeak (0);
            if (band < 0 || raw (proc, eqBandParamId (band, pid::eqBandOn)) < 0.5f
                || std::abs (std::log2 (raw (proc, eqBandParamId (band, pid::eqBandFreq)) / 3000.0f)) > 0.05f
                || ! (raw (proc, eqBandParamId (band, pid::eqBandGain)) < -3.0f) || raw (proc, eqBandParamId (band, pid::eqBandQ)) < 2.0f)
                fail ("fixing the 3 kHz peak didn't put a narrow cut there");
        }
    }

    // --- A band's tab: double-click switches it; Esc closes ----------------------------------------
    {
        const float before = raw (proc, eqBandParamId (7, pid::eqBandOn));
        overlay->getBandTab (7).mouseDoubleClick (mouseAt (overlay->getBandTab (7), { 5.0f, 5.0f }, {}, 2));
        if (juce::exactlyEqual (raw (proc, eqBandParamId (7, pid::eqBandOn)), before))
            fail ("double-clicking band 8's tab didn't switch it");
        overlay->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        if (ed->isEqVisible())
            fail ("Esc didn't close the EQ");
    }

    editor = nullptr;
    std::printf ("eq editor: open, node drag (undo), scroll Q, add / remove bands, listen sweep and hold, find peaks, band tabs, Esc %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

} // namespace drum::eqchecks
