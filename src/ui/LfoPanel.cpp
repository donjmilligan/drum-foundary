// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "LfoPanel.h"
#include "FoundryLookAndFeel.h"
#include "../dsp/Lfo.h"

namespace drum::ui
{

namespace
{
    constexpr int kPadding = 10;
    constexpr int kHeaderHeight = 26;
    constexpr int kMiddleHeight = 92;
    constexpr int kRowHeight = 21, kRowGap = 2;

    float raw (APVTS& state, const juce::String& id)
    {
        const auto* v = state.getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    }

    void setPlain (APVTS& state, const juce::String& id, float plain)
    {
        if (auto* p = state.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
            p->endChangeGesture();
        }
    }

    /** The group a target goes under in a slot's menu, from its name ("Body Level" -> "Body"). */
    juce::String targetGroup (const juce::String& name)
    {
        if (name.startsWith ("Layer "))
            return name.substring (0, 7); // "Layer 1"
        for (auto* prefix : { "Body", "Noise", "Click", "Reverb", "EQ" })
            if (name.startsWith (prefix))
                return prefix;
        if (name.startsWith ("Gate"))
            return "Reverb";
        if (name.startsWith ("Drive"))
            return "Drive";
        if (name.startsWith ("Output") || name.startsWith ("Clip"))
            return "Output";
        return "Global";
    }

    /** A repeatable random value per cycle, for drawing S&H and Drift. */
    float cycleRandom (int lfo, int cycle) noexcept
    {
        const auto h = dsp::mixSeed ((uint32_t) cycle * 0x9E3779B9u, 0xA5u + (uint32_t) lfo);
        return (float) (h >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
}

//==============================================================================
/** "LFO 1" with a grip: drag it onto a knob. */
class LfoPage::Handle : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Handle (int lfoIndex) : index (lfoIndex)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag LFO " + juce::String (index + 1) + " onto any knob to modulate it");
        setRepaintsOnMouseActivity (true);
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = colours::lfo (index);
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (c.withAlpha (isMouseOver() ? 0.28f : 0.16f));
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (c);
        g.drawRoundedRectangle (bounds, 5.0f, 1.2f);

        // A grip: two columns of dots.
        auto area = bounds.reduced (8.0f, 0.0f);
        const auto grip = area.removeFromLeft (7.0f);
        for (int col = 0; col < 2; ++col)
            for (int row = -1; row <= 1; ++row)
                g.fillEllipse (juce::Rectangle<float> (2.2f, 2.2f).withCentre ({ grip.getX() + 1.2f + (float) col * 4.0f,
                                                                                 grip.getCentreY() + (float) row * 4.0f }));
        area.removeFromLeft (6.0f);
        g.setFont (uiFont (12.5f, true));
        g.drawText ("LFO " + juce::String (index + 1), area, juce::Justification::centredLeft, false);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 4)
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! container->isDragAndDropActive())
                container->startDragging (lfoDragDescription (index), this);
    }

private:
    const int index;
};

//==============================================================================
/** What the LFO does over one hit: its wave across Length, with the playhead and its
    live output while a hit plays. */
class LfoPage::Preview : public juce::Component
{
public:
    Preview (DrumFoundryProcessor& p, int lfoIndex) : processor (p), index (lfoIndex) {}

    void paint (juce::Graphics& g) override
    {
        const auto c = colours::lfo (index);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours::display);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (colours::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);

        const auto plot = bounds.reduced (6.0f, 16.0f).withTrimmedTop (2.0f);
        g.setColour (colours::gridStrong);
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());

        const auto params = processor.readParams();
        const auto& l = params.lfos[(size_t) index];
        const double cps = LfoBank::cyclesPerSecond (l, params.bpm);
        const double cycles = cps * (double) params.lengthMs * 0.001;
        const bool oneShot = l.mode == 1, free = l.mode == 2;

        const auto valueAt = [&] (double cyc)
        {
            if (oneShot && cyc >= 1.0)
                cyc = 0.99999;
            const double t = cyc + (double) l.phase;
            const int k = (int) std::floor (t);
            return LfoBank::shapeValue (l.shape, (float) (t - std::floor (t)), cycleRandom (index, k), cycleRandom (index, k - 1));
        };
        const auto yOf = [&] (float v) { return plot.getCentreY() - v * plot.getHeight() * 0.5f; };

        juce::Path wave;
        const int steps = juce::jmax (2, (int) plot.getWidth() * 2);
        for (int i = 0; i <= steps; ++i)
        {
            const double frac = (double) i / steps;
            const float x = plot.getX() + (float) frac * plot.getWidth();
            const float y = yOf (valueAt (frac * cycles));
            if (i == 0)
                wave.startNewSubPath (x, y);
            else
                wave.lineTo (x, y);
        }
        g.setColour (c.withAlpha (free ? 0.5f : 0.95f));
        g.strokePath (wave, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Where the latest hit is, and what the LFO puts out now.
        if (const float pos = processor.getPlayhead(); pos >= 0.0f && pos <= 1.0f)
        {
            const float x = plot.getX() + pos * plot.getWidth();
            g.setColour (colours::text.withAlpha (0.35f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            const float y = yOf (processor.getLfoValue (index));
            g.setColour (c);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ x, y }));
        }

        g.setFont (uiFont (10.5f));
        g.setColour (colours::textDim);
        const auto top = bounds.reduced (7.0f, 2.0f).withHeight (13.0f);
        const auto cycleMs = cps > 0.0 ? 1000.0 / cps : 0.0;
        g.drawText (free    ? juce::String ("free running")
                  : oneShot ? "one " + juce::String (juce::roundToInt (cycleMs)) + " ms cycle, then holds"
                            : juce::String (cycles, cycles < 10.0 ? 1 : 0) + " cycles per hit",
                    top, juce::Justification::centredLeft, false);
        const auto rate = l.sync > 0 ? lfoSyncNames[l.sync] + " @ " + juce::String (juce::roundToInt (params.bpm)) + " BPM"
                                     : juce::String (cps, cps < 10.0 ? 2 : 1) + " Hz";
        g.drawText (rate, top, juce::Justification::centredRight, false);
        g.drawText (juce::String (juce::roundToInt (params.lengthMs)) + " ms",
                    bounds.reduced (7.0f, 2.0f).removeFromBottom (13.0f), juce::Justification::centredRight, false);
    }

private:
    DrumFoundryProcessor& processor;
    const int index;
};

//==============================================================================
/** One target slot: what it modulates (click for a menu), its Amount, and x to clear it. */
class LfoPage::SlotRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    SlotRow (APVTS& s, int lfoIndex, int slotIndex) : state (s), lfo (lfoIndex), slot (slotIndex)
    {
        amount.setSliderStyle (juce::Slider::LinearBar);
        amount.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        amount.setColour (juce::Slider::trackColourId, colours::lfo (lfo));
        amount.getProperties().set ("bipolar", true);
        amount.getProperties().set ("caption", "AMOUNT");
        amount.setTooltip ("How far LFO " + juce::String (lfo + 1) + " moves it: up to the whole range either way (double-click: 0)");
        attachment = std::make_unique<APVTS::SliderAttachment> (state, lfoParamId (lfo, pid::lfoAmt, slot), amount);
        amount.setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (amount);

        clear.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
        clear.setTooltip ("Stop modulating it");
        clear.onClick = [this] { clearLfoSlot (state, lfo, slot); };
        addAndMakeVisible (clear);

        setRepaintsOnMouseActivity (true);
    }

    /** Re-reads the slot's target. `firstEmpty`: the first unused slot, which says how to fill it. */
    void update (bool firstEmpty)
    {
        const int dest = juce::roundToInt (raw (state, lfoParamId (lfo, pid::lfoDest, slot)));
        if (dest == shownDest && firstEmpty == shownFirstEmpty)
            return;

        shownDest = dest;
        shownFirstEmpty = firstEmpty;
        const bool used = dest > 0;
        amount.setVisible (used);
        clear.setVisible (used);
        setTooltip (used ? "LFO " + juce::String (lfo + 1) + " modulates " + modTargetChoiceNames()[dest] + ". Click to change it."
                         : "Click to pick what LFO " + juce::String (lfo + 1) + " modulates, or drag its handle onto a knob");
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = colours::lfo (lfo);
        const bool used = shownDest > 0;
        const bool hover = isMouseOver (false) && nameArea.contains (getMouseXYRelative());

        auto box = nameArea.toFloat().reduced (0.5f);
        g.setColour (hover ? colours::panelBorder : colours::display);
        g.fillRoundedRectangle (box, 4.0f);
        if (used)
        {
            g.setColour (c);
            g.fillRoundedRectangle (box.removeFromLeft (3.0f), 1.5f);
        }

        g.setFont (uiFont (11.5f, used));
        g.setColour (used ? colours::text : colours::textDim.withAlpha (shownFirstEmpty ? 1.0f : 0.5f));
        const auto text = used ? modTargetChoiceNames()[shownDest]
                               : (shownFirstEmpty ? "Drag LFO " + juce::String (lfo + 1) + " onto a knob"
                                                  : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")));
        g.drawFittedText (text, nameArea.reduced (8, 0), juce::Justification::centredLeft, 1, 0.85f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        if (shownDest > 0)
        {
            clear.setBounds (area.removeFromRight (20));
            area.removeFromRight (3);
            amount.setBounds (area.removeFromRight (area.getWidth() / 2));
            area.removeFromRight (4);
        }
        nameArea = area;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (nameArea.contains (e.getPosition()))
            showTargetMenu();
    }

private:
    void showTargetMenu()
    {
        const auto& names = modTargetChoiceNames();
        juce::PopupMenu menu;
        menu.addSectionHeader ("LFO " + juce::String (lfo + 1) + " modulates");
        menu.addItem ("Nothing", shownDest > 0, shownDest <= 0, [this] { clearLfoSlot (state, lfo, slot); });

        // One submenu per group, in the targets' order.
        juce::StringArray groups;
        std::vector<juce::PopupMenu> menus;
        for (int i = 1; i < names.size(); ++i)
        {
            const auto group = targetGroup (names[i]);
            int g = groups.indexOf (group);
            if (g < 0)
            {
                groups.add (group);
                menus.emplace_back();
                g = groups.size() - 1;
            }
            const bool elsewhere = findLfoSlot (state, lfo, i - 1) >= 0 && i != shownDest;
            menus[(size_t) g].addItem (names[i], ! elsewhere, i == shownDest, [this, i] { setTarget (i); });
        }
        for (int g = 0; g < groups.size(); ++g)
            menu.addSubMenu (groups[g], menus[(size_t) g]);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (180));
    }

    void setTarget (int dest)
    {
        setPlain (state, lfoParamId (lfo, pid::lfoDest, slot), (float) dest);
        if (juce::exactlyEqual (raw (state, lfoParamId (lfo, pid::lfoAmt, slot)), 0.0f))
            setPlain (state, lfoParamId (lfo, pid::lfoAmt, slot), 0.25f);
    }

    APVTS& state;
    const int lfo, slot;
    juce::Slider amount;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
    juce::TextButton clear;
    juce::Rectangle<int> nameArea;
    int shownDest = -1;
    bool shownFirstEmpty = false;
};

//==============================================================================
/** One LFO. */
class LfoPage::Panel : public juce::Component
{
public:
    Panel (DrumFoundryProcessor& p, int lfoIndex)
        : processor (p), index (lfoIndex), handle (lfoIndex), preview (p, lfoIndex),
          shape (p.getState(), lfoParamId (lfoIndex, pid::lfoShape), {}, colours::lfo (lfoIndex)),
          mode (p.getState(), lfoParamId (lfoIndex, pid::lfoMode), {}, colours::lfo (lfoIndex)),
          sync (p.getState(), lfoParamId (lfoIndex, pid::lfoSync), "Sync", colours::lfo (lfoIndex)),
          rate (p.getState(), lfoParamId (lfoIndex, pid::lfoRate), "Rate", colours::lfo (lfoIndex)),
          phase (p.getState(), lfoParamId (lfoIndex, pid::lfoPhase), "Phase", colours::lfo (lfoIndex))
    {
        shape.getComboBox().setTooltip ("The LFO's wave. S&H holds a random value each cycle; Drift glides between them.");
        mode.getComboBox().setTooltip ("Retrig: every hit starts the LFO at its Phase\n"
                                       "One Shot: the same, but it runs one cycle and holds (an extra envelope)\n"
                                       "Free: it runs on its own, never restarting");
        sync.getComboBox().setTooltip ("Free: Rate sets the speed. Otherwise one cycle per that note length at the host's tempo.");
        rate.getSlider().setTooltip ("Rate: cycles per second (when Sync is Free)");
        phase.getSlider().setTooltip ("Phase: where in its cycle a hit starts the LFO");

        for (auto* c : std::initializer_list<juce::Component*> { &handle, &preview, &shape, &mode, &sync, &rate, &phase })
            addAndMakeVisible (c);

        for (int k = 0; k < kLfoSlots; ++k)
        {
            slots[(size_t) k] = std::make_unique<SlotRow> (p.getState(), index, k);
            addAndMakeVisible (*slots[(size_t) k]);
        }
    }

    Handle& getHandle() noexcept { return handle; }

    void update()
    {
        auto& state = processor.getState();
        bool firstEmpty = true;
        for (int k = 0; k < kLfoSlots; ++k)
        {
            const bool empty = juce::roundToInt (raw (state, lfoParamId (index, pid::lfoDest, k))) <= 0;
            slots[(size_t) k]->update (empty && firstEmpty);
            if (empty)
                firstEmpty = false;
        }

        // Rate only counts while Sync is Free.
        const bool synced = juce::roundToInt (raw (state, lfoParamId (index, pid::lfoSync))) > 0;
        rate.setAlpha (synced ? 0.4f : 1.0f);
        preview.repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (colours::panel);
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (colours::panelBorder);
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (kPadding, 8);

        auto header = area.removeFromTop (kHeaderHeight);
        handle.setBounds (header.removeFromLeft (82));
        header.removeFromLeft (8);
        shape.setBounds (header.removeFromLeft (juce::jmin (112, header.getWidth() / 2)).reduced (0, 1));
        header.removeFromLeft (6);
        mode.setBounds (header.removeFromLeft (juce::jmin (104, header.getWidth())).reduced (0, 1));
        area.removeFromTop (6);

        auto middle = area.removeFromTop (kMiddleHeight);
        constexpr int knobWidth = 64, syncWidth = 84;
        phase.setBounds (middle.removeFromRight (knobWidth));
        sync.setBounds (middle.removeFromRight (syncWidth));
        rate.setBounds (middle.removeFromRight (knobWidth));
        middle.removeFromRight (6);
        preview.setBounds (middle);
        area.removeFromTop (6);

        for (auto& row : slots)
        {
            row->setBounds (area.removeFromTop (kRowHeight));
            area.removeFromTop (kRowGap);
        }
    }

private:
    DrumFoundryProcessor& processor;
    const int index;
    Handle handle;
    Preview preview;
    ChoiceSelector shape, mode, sync;
    Knob rate, phase;
    std::array<std::unique_ptr<SlotRow>, kLfoSlots> slots;
};

//==============================================================================
LfoPage::LfoPage (DrumFoundryProcessor& p)
{
    for (int i = 0; i < kNumLfos; ++i)
    {
        panels[(size_t) i] = std::make_unique<Panel> (p, i);
        addAndMakeVisible (*panels[(size_t) i]);
    }
}

LfoPage::~LfoPage()
{
    stopTimer();
}

juce::Component& LfoPage::getHandle (int index)
{
    return panels[(size_t) juce::jlimit (0, kNumLfos - 1, index)]->getHandle();
}

void LfoPage::resized()
{
    constexpr int gap = 8;
    auto area = getLocalBounds();
    const int w = (area.getWidth() - gap * (kNumLfos - 1)) / kNumLfos;
    for (int i = 0; i < kNumLfos; ++i)
    {
        panels[(size_t) i]->setBounds (i + 1 == kNumLfos ? area : area.removeFromLeft (w));
        area.removeFromLeft (gap);
    }
}

void LfoPage::visibilityChanged()
{
    if (isVisible())
    {
        timerCallback();
        startTimerHz (30);
    }
    else
    {
        stopTimer();
    }
}

void LfoPage::timerCallback()
{
    for (auto& panel : panels)
        panel->update();
}

} // namespace drum::ui
