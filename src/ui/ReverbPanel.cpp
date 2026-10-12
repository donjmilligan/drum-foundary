// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "ReverbPanel.h"
#include "FoundryLookAndFeel.h"
#include "../dsp/Reverb.h"

namespace drum::ui
{

namespace
{
    constexpr double kRenderRate = 48000.0;
    constexpr float kFloorDb = -60.0f;
    const juce::String midDot (juce::CharPointer_UTF8 ("\xc2\xb7"));
}

//==============================================================================
ReverbResponse::ReverbResponse (DrumFoundryProcessor& p)
    : juce::Thread ("Drum Foundry reverb response"), processor (p)
{
    setInterceptsMouseClicks (false, false);
    startThread (juce::Thread::Priority::low);
}

ReverbResponse::~ReverbResponse()
{
    stopTimer();
    signalThreadShouldExit();
    notify();
    stopThread (4000);
}

void ReverbResponse::visibilityChanged()
{
    if (isVisible())
    {
        startTimerHz (15);
        timerCallback();
    }
    else
        stopTimer();
}

bool ReverbResponse::sameReverb (const DrumParams& a, const DrumParams& b) noexcept
{
    return a.revEngine == b.revEngine && a.revGate == b.revGate
        && juce::exactlyEqual (a.revSize, b.revSize) && juce::exactlyEqual (a.revDecayMs, b.revDecayMs)
        && juce::exactlyEqual (a.revPreDelayMs, b.revPreDelayMs) && juce::exactlyEqual (a.revLowCut, b.revLowCut)
        && juce::exactlyEqual (a.revHighCut, b.revHighCut) && juce::exactlyEqual (a.revGateMs, b.revGateMs)
        && juce::exactlyEqual (a.revFadeMs, b.revFadeMs) && juce::exactlyEqual (a.revDiffusion, b.revDiffusion)
        && juce::exactlyEqual (a.revModDepth, b.revModDepth) && juce::exactlyEqual (a.revModRate, b.revModRate)
        && juce::exactlyEqual (a.revWidth, b.revWidth) && juce::exactlyEqual (a.revEarly, b.revEarly)
        && juce::exactlyEqual (a.revDamp, b.revDamp);
}

bool ReverbResponse::isUpToDate() const noexcept
{
    const auto& r = render; // message thread only
    return r != nullptr && sameReverb (r->params, processor.readParams());
}

void ReverbResponse::timerCallback()
{
    const auto p = processor.readParams();
    {
        const std::lock_guard<std::mutex> l (lock);
        if (haveWanted && sameReverb (p, wanted))
            return;
        wanted = p;
        haveWanted = true;
    }
    notify();
}

std::shared_ptr<ReverbResponse::Render> ReverbResponse::makeRender (const DrumParams& p, Reverb& reverb,
                                                                    const std::function<bool()>& shouldStop)
{
    constexpr int block = 512;
    std::vector<float> l (block), r (block);

    // The impulse response at Mix 100%: an impulse in, the wet part out (what's
    // added to the dry), with the hit at the impulse for the gate.
    auto q = p;
    q.revMix = 1.0f;
    q.revOn = true;
    const double seconds = juce::jlimit (0.3, 8.0, Reverb::tailSeconds (q) + 0.25);
    const int total = (int) (seconds * kRenderRate);
    const int perMs = (int) (kRenderRate / 1000.0);

    auto out = std::make_shared<Render>();
    out->params = p;
    std::vector<float> wet ((size_t) total * 2);
    reverb.reset();
    for (int pos = 0; pos < total && ! shouldStop(); pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        if (pos == 0)
            l[0] = r[0] = 1.0f;
        const int hit = 0;
        reverb.process (l.data(), r.data(), n, q, pos == 0 ? &hit : nullptr, pos == 0 ? 1 : 0, 0);
        if (pos == 0)
            l[0] -= 1.0f, r[0] -= 1.0f; // wet only
        for (int i = 0; i < n; ++i)
        {
            wet[(size_t) (pos + i) * 2] = l[(size_t) i];
            wet[(size_t) (pos + i) * 2 + 1] = r[(size_t) i];
        }
    }
    if (shouldStop())
        return nullptr;

    // Per millisecond: peak and waveform span; then the RT60 from the backward-
    // integrated energy (Schroeder), from -5 to -25 dB.
    const int ms = total / perMs;
    out->peak.resize ((size_t) ms);
    out->lo.resize ((size_t) ms);
    out->hi.resize ((size_t) ms);
    for (int k = 0; k < ms; ++k)
    {
        float pk = 0.0f, lo = 0.0f, hi = 0.0f;
        for (int i = k * perMs; i < (k + 1) * perMs; ++i)
            for (int c = 0; c < 2; ++c)
            {
                const float v = wet[(size_t) i * 2 + (size_t) c];
                pk = juce::jmax (pk, std::abs (v));
                lo = juce::jmin (lo, v);
                hi = juce::jmax (hi, v);
            }
        out->peak[(size_t) k] = pk;
        out->lo[(size_t) k] = lo;
        out->hi[(size_t) k] = hi;
    }
    std::vector<double> edc ((size_t) total + 1, 0.0);
    for (int i = total; --i >= 0;)
        edc[(size_t) i] = edc[(size_t) i + 1] + (double) wet[(size_t) i * 2] * wet[(size_t) i * 2]
                        + (double) wet[(size_t) i * 2 + 1] * wet[(size_t) i * 2 + 1];
    int a = -1, b = -1;
    for (int i = 0; i < total && edc[0] > 0.0; ++i)
    {
        const double db = 10.0 * std::log10 (edc[(size_t) i] / edc[0] + 1.0e-30);
        if (a < 0 && db <= -5.0)
            a = i;
        if (b < 0 && db <= -25.0)
        {
            b = i;
            break;
        }
    }
    out->rt60 = a >= 0 && b > a && ! q.revGate ? (float) (3.0 * (b - a) / kRenderRate) : 0.0f;

    return out;
}

void ReverbResponse::renderNow()
{
    Reverb reverb;
    reverb.prepare (kRenderRate);
    render = makeRender (processor.readParams(), reverb, [] { return false; });
    measuredRt60.store (render != nullptr ? render->rt60 : 0.0f);
    repaint();
}

void ReverbResponse::run()
{
    Reverb reverb;
    reverb.prepare (kRenderRate);

    while (! threadShouldExit())
    {
        wait (-1);
        if (threadShouldExit())
            break;

        DrumParams p;
        {
            const std::lock_guard<std::mutex> g (lock);
            if (! haveWanted)
                continue;
            p = wanted;
        }

        auto out = makeRender (p, reverb, [this] { return threadShouldExit(); });
        if (out == nullptr)
            break;

        juce::Component::SafePointer<ReverbResponse> safe (this);
        juce::MessageManager::callAsync ([safe, out]
        {
            if (safe == nullptr)
                return;
            safe->render = out;
            safe->measuredRt60.store (out->rt60);
            safe->repaint();
        });
    }
}

void ReverbResponse::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 6.0f);

    auto plot = bounds.reduced (12.0f, 10.0f).withTrimmedLeft (34.0f).withTrimmedBottom (18.0f);
    const auto r = render;

    // dB grid
    g.setFont (uiFont (10.0f));
    for (float db : { 0.0f, -20.0f, -40.0f, -60.0f })
    {
        const float y = plot.getY() + (-db / -kFloorDb) * plot.getHeight();
        g.setColour (colours::grid);
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (colours::textDim);
        g.drawText (db == 0.0f ? "0 dB" : juce::String ((int) db), juce::Rectangle<float> (bounds.getX() + 4.0f, y - 7.0f, 40.0f, 14.0f),
                    juce::Justification::centredLeft);
    }

    if (r == nullptr || r->peak.empty())
    {
        g.setColour (colours::textDim);
        g.drawText ("Rendering" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")), plot, juce::Justification::centred);
        return;
    }

    const int ms = (int) r->peak.size();
    const auto xOf = [&] (float t) { return plot.getX() + plot.getWidth() * t / (float) ms; };
    const float maxPeak = juce::jmax (1.0e-9f, *std::max_element (r->peak.begin(), r->peak.end()));
    const auto yOfDb = [&] (float db) { return plot.getY() + juce::jlimit (0.0f, 1.0f, db / kFloorDb) * plot.getHeight(); };

    // Time grid: every 100 ms (or 500 ms past two seconds).
    const int step = ms > 2000 ? 500 : 100;
    for (int t = step; t < ms; t += step)
    {
        const float x = xOf ((float) t);
        g.setColour (colours::grid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        if (t % (step * 2) == 0 || ms <= 1000)
        {
            g.setColour (colours::textDim);
            g.drawText (t >= 1000 ? juce::String (t / 1000.0, 1) + " s" : juce::String (t) + " ms",
                        juce::Rectangle<float> (x - 30.0f, plot.getBottom() + 3.0f, 60.0f, 13.0f), juce::Justification::centred);
        }
    }

    // The waveform, faint, around the middle (scaled to its own peak).
    {
        juce::Path wave;
        const float cy = plot.getCentreY(), amp = plot.getHeight() * 0.45f / maxPeak;
        const int columns = juce::jmax (1, (int) plot.getWidth());
        std::vector<float> top ((size_t) columns), bottom ((size_t) columns);
        for (int c = 0; c < columns; ++c)
        {
            const int k0 = c * ms / columns, k1 = juce::jmax (k0 + 1, (c + 1) * ms / columns);
            float lo = 0.0f, hi = 0.0f;
            for (int k = k0; k < juce::jmin (ms, k1); ++k)
            {
                lo = juce::jmin (lo, r->lo[(size_t) k]);
                hi = juce::jmax (hi, r->hi[(size_t) k]);
            }
            top[(size_t) c] = cy - hi * amp - 0.5f;
            bottom[(size_t) c] = cy - lo * amp + 0.5f;
        }
        wave.startNewSubPath (plot.getX(), top[0]);
        for (int c = 1; c < columns; ++c)
            wave.lineTo (plot.getX() + (float) c, top[(size_t) c]);
        for (int c = columns; --c >= 0;)
            wave.lineTo (plot.getX() + (float) c, bottom[(size_t) c]);
        wave.closeSubPath();
        g.setColour (colours::waveform.withAlpha (0.55f));
        g.fillPath (wave);
    }

    // The level envelope (dB under its own peak), smoothed over ~10 ms, filled down to the floor.
    std::vector<float> smooth ((size_t) ms);
    {
        constexpr int half = 5;
        for (int k = 0; k < ms; ++k)
        {
            float m = 0.0f;
            for (int j = juce::jmax (0, k - half); j <= juce::jmin (ms - 1, k + half); ++j)
                m += r->peak[(size_t) j];
            smooth[(size_t) k] = m / (float) (juce::jmin (ms - 1, k + half) - juce::jmax (0, k - half) + 1);
        }
    }
    {
        juce::Path env;
        env.startNewSubPath (plot.getX(), plot.getBottom());
        for (int k = 0; k < ms; ++k)
            env.lineTo (xOf ((float) k + 0.5f), yOfDb (juce::Decibels::gainToDecibels (smooth[(size_t) k] / maxPeak, kFloorDb)));
        env.lineTo (plot.getRight(), plot.getBottom());
        env.closeSubPath();
        g.setColour (colours::reverb.withAlpha (0.16f));
        g.fillPath (env);
        g.setColour (colours::reverb.withAlpha (0.9f));
        juce::Path line;
        for (int k = 0; k < ms; ++k)
        {
            const juce::Point<float> pt (xOf ((float) k + 0.5f), yOfDb (juce::Decibels::gainToDecibels (smooth[(size_t) k] / maxPeak, kFloorDb)));
            if (k == 0)
                line.startNewSubPath (pt);
            else
                line.lineTo (pt);
        }
        g.strokePath (line, juce::PathStrokeType (1.4f));
    }

    const auto& p = r->params;
    const float dashes[] { 4.0f, 3.0f };

    // What Decay asks for: 60 dB down over Decay, from the pre-delay.
    {
        const float x0 = xOf (p.revPreDelayMs), x1 = xOf (p.revPreDelayMs + p.revDecayMs);
        g.setColour (colours::text.withAlpha (0.45f));
        g.drawDashedLine ({ x0, yOfDb (0.0f), juce::jmin (x1, plot.getRight()),
                            yOfDb (kFloorDb * juce::jmin (1.0f, (plot.getRight() - x0) / juce::jmax (1.0f, x1 - x0))) },
                          dashes, 2, 1.0f);
    }

    // Pre-delay.
    if (p.revPreDelayMs > 0.5f)
    {
        g.setColour (colours::textDim.withAlpha (0.8f));
        g.drawDashedLine ({ xOf (p.revPreDelayMs), plot.getY(), xOf (p.revPreDelayMs), plot.getBottom() }, dashes, 2, 1.0f);
        g.drawText ("PRE " + juce::String ((int) p.revPreDelayMs) + " ms",
                    juce::Rectangle<float> (xOf (p.revPreDelayMs) + 4.0f, plot.getY() + 2.0f, 80.0f, 12.0f),
                    juce::Justification::centredLeft);
    }

    // The gate: its cut, its fade, and what it leaves out.
    if (p.revGate)
    {
        const float xCut = xOf (p.revGateMs), xEnd = xOf (p.revGateMs + p.revFadeMs);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (xEnd, plot.getY(), juce::jmax (0.0f, plot.getRight() - xEnd), plot.getHeight()));
        g.setColour (colours::reverb.withAlpha (0.85f));
        g.drawVerticalLine (juce::roundToInt (xCut), plot.getY(), plot.getBottom());
        g.drawLine (xCut, plot.getY(), xEnd, plot.getBottom(), 1.0f);
        g.setFont (uiFont (10.0f, true));
        g.drawText ("GATE " + juce::String ((int) p.revGateMs) + " ms",
                    juce::Rectangle<float> (xCut + 4.0f, plot.getY() + 16.0f, 90.0f, 12.0f), juce::Justification::centredLeft);
    }

    // Readout.
    g.setFont (uiFont (11.0f, true));
    g.setColour (colours::text.withAlpha (0.85f));
    const auto engineName = revEngineNames[juce::jlimit (0, revEngineNames.size() - 1, p.revEngine)].toUpperCase();
    const auto readout = r->rt60 > 0.0f ? engineName + "   RT60 " + juce::String (r->rt60, 2) + " s  (DECAY " + juce::String (p.revDecayMs / 1000.0f, 2) + " s)"
                                        : engineName + "   DECAY " + juce::String (p.revDecayMs / 1000.0f, 2) + " s";
    g.drawText (readout, plot.reduced (8.0f, 4.0f), juce::Justification::topRight);
}

//==============================================================================
ReverbOverlay::ReverbOverlay (DrumFoundryProcessor& p)
    : processor (p), power (p.getState(), pid::revOn, colours::reverb, "Reverb"),
      engine (p.getState(), pid::revEngine, {}, colours::reverb), response (p)
{
    engine.getComboBox().setTooltip ("Engine:\nCloud: Ghost Note Audio's Cloud Seed algorithm (MIT): diffuse, modulated, wide\n"
                                     "Classic: the original 8-line reverb (sessions from before Cloud use it)\n"
                                     "Natural: a room built from shaped noise and convolved: organic, no metallic ring, best for small rooms");
    closeButton.setTooltip ("Back to the main view (Esc)");
    closeButton.onClick = [this] { if (onClose) onClose(); };

    auto& mix = knob (pid::revMix, "Mix");
    auto& size = knob (pid::revSize, "Size");
    auto& decay = knob (pid::revDecay, "Decay");
    auto& pre = knob (pid::revPreDelay, "Pre-Delay");
    auto& diffusion = knob (pid::revDiffusion, "Diffusion");
    auto& lowCut = knob (pid::revLowCut, "Low Cut");
    auto& highCut = knob (pid::revHighCut, "High Cut");
    auto& damping = knob (pid::revDamp, "Damping");
    auto& depth = knob (pid::revModDepth, "Mod Depth");
    auto& rate = knob (pid::revModRate, "Mod Rate");
    auto& width = knob (pid::revWidth, "Width");
    auto& early = knob (pid::revEarly, "Early");
    auto& gateTime = knob (pid::revGateTime, "Time");
    auto& gateFade = knob (pid::revGateFade, "Fade");
    gateSwitch = std::make_unique<ToggleSwitch> (p.getState(), pid::revGate, "GATE", colours::reverb,
                                                 "Cut the reverb tail Time after each hit, fading out over Fade");

    size.getSlider().setTooltip ("Size: how big the space is (the delay lines' length)");
    decay.getSlider().setTooltip ("Decay: how long the tail takes to fall 60 dB (RT60). The dashed line on the graph is what it asks for");
    diffusion.getSlider().setTooltip ("Diffusion (Cloud, Natural): how quickly echoes blur into a smooth wash");
    damping.getSlider().setTooltip ("Damping (Cloud, Natural): highs die faster than lows, like a real room");
    depth.getSlider().setTooltip ("Mod Depth (Cloud): slow movement in the delays, for a lush, unmetallic tail");
    rate.getSlider().setTooltip ("Mod Rate (Cloud): how fast that movement is");
    width.getSlider().setTooltip ("Width (Cloud, Natural): from mono to fully decorrelated left and right");
    early.getSlider().setTooltip ("Early (Cloud, Natural): the early reflections' level next to the tail");

    groups = { { "SPACE", { &size, &decay, &pre, &diffusion }, {} },
               { "TONE", { &lowCut, &highCut, &damping }, {} },
               { "MOTION & WIDTH", { &depth, &rate, &width, &early }, {} },
               { "GATE", { gateSwitch.get(), &gateTime, &gateFade }, {} },
               { "MIX", { &mix }, {} } };
    cloudOnly = { &depth, &rate };
    notClassic = { &diffusion, &damping, &width, &early };

    for (auto* c : std::initializer_list<juce::Component*> { &power, &engine, &closeButton, &response })
        addAndMakeVisible (c);
    for (auto& grp : groups)
        for (auto* c : grp.items)
            addAndMakeVisible (c);

    setWantsKeyboardFocus (true);
    setVisible (false);
}

ReverbOverlay::~ReverbOverlay()
{
    stopTimer();
}

Knob& ReverbOverlay::knob (const juce::String& id, const char* caption)
{
    knobs.push_back (std::make_unique<Knob> (processor.getState(), id, caption, colours::reverb));
    return *knobs.back();
}

void ReverbOverlay::visibilityChanged()
{
    if (isVisible())
    {
        startTimerHz (10);
        timerCallback();
    }
    else
        stopTimer();
}

void ReverbOverlay::timerCallback()
{
    const int e = processor.readParams().revEngine;
    if (e != lastEngine)
    {
        lastEngine = e;
        for (auto* c : cloudOnly)
            c->setAlpha (e == revEngineCloud ? 1.0f : 0.35f);
        for (auto* c : notClassic)
            c->setAlpha (e != revEngineClassic ? 1.0f : 0.35f);
        repaint();
    }
}

juce::Rectangle<int> ReverbOverlay::boxBounds() const
{
    return getLocalBounds().reduced (14);
}

void ReverbOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.7f));
    const auto box = boxBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

    g.setFont (titleFont (15.0f));
    g.setColour (colours::reverb);
    g.drawText ("REVERB", titleArea, juce::Justification::centredLeft);
    g.setFont (uiFont (11.5f));
    g.setColour (colours::textDim);
    g.drawText ("ENGINE", engineLabelArea, juce::Justification::centredRight);
    if (lastEngine == revEngineCloud)
    {
        g.setFont (uiFont (10.5f));
        g.drawText ("Cloud Seed algorithm " + midDot + " Ghost Note Audio " + midDot + " MIT License", creditArea,
                    juce::Justification::centredLeft);
    }
    else if (lastEngine == revEngineNatural)
    {
        g.setFont (uiFont (10.5f));
        g.drawText ("A room of shaped noise " + midDot + " convolved", creditArea, juce::Justification::centredLeft);
    }

    for (const auto& grp : groups)
    {
        const auto a = grp.area.toFloat();
        g.setColour (colours::panelBorder);
        g.drawRoundedRectangle (a.reduced (0.5f), 6.0f, 1.0f);
        g.setFont (uiFont (10.5f, true));
        g.setColour (colours::reverb.withAlpha (0.85f));
        g.drawText (grp.title, a.reduced (10.0f, 4.0f).removeFromTop (14.0f), juce::Justification::centredLeft);
    }
}

void ReverbOverlay::resized()
{
    auto box = boxBounds().reduced (16, 12);

    auto top = box.removeFromTop (30);
    power.setBounds (top.removeFromLeft (22).withSizeKeepingCentre (20, 20));
    top.removeFromLeft (8);
    titleArea = top.removeFromLeft (80);
    closeButton.setBounds (top.removeFromRight (70).reduced (0, 2));
    top.removeFromRight (16);
    engine.setBounds (top.removeFromRight (110).reduced (0, 2));
    engineLabelArea = top.removeFromRight (60);
    top.removeFromRight (12);
    creditArea = top;

    box.removeFromTop (10);
    auto controls = box.removeFromBottom (juce::jmin (190, box.getHeight() / 2));
    box.removeFromBottom (10);
    response.setBounds (box);

    // Groups, sized by how many controls they hold.
    int slots = 0;
    for (const auto& grp : groups)
        slots += (int) grp.items.size();
    constexpr int gap = 8;
    const float unit = (float) (controls.getWidth() - gap * ((int) groups.size() - 1)) / (float) juce::jmax (1, slots);
    for (size_t k = 0; k < groups.size(); ++k)
    {
        auto& grp = groups[k];
        const int w = k + 1 == groups.size() ? controls.getWidth() : juce::roundToInt (unit * (float) grp.items.size());
        grp.area = controls.removeFromLeft (w);
        controls.removeFromLeft (gap);

        auto inner = grp.area.reduced (6, 6).withTrimmedTop (16);
        const int cell = inner.getWidth() / juce::jmax (1, (int) grp.items.size());
        for (auto* c : grp.items)
        {
            auto slot = inner.removeFromLeft (cell);
            if (c == gateSwitch.get())
                c->setBounds (slot.withSizeKeepingCentre (juce::jmin (slot.getWidth() - 8, 76), 26));
            else
                c->setBounds (slot);
        }
    }
}

void ReverbOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && ! boxBounds().contains (e.getPosition()) && onClose)
        onClose();
}

bool ReverbOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();
        return true;
    }
    return false;
}

} // namespace drum::ui
