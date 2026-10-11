// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "FxChainStrip.h"
#include "FoundryLookAndFeel.h"

namespace drum::ui
{

namespace
{
    constexpr float kChipWidth = 92.0f;
    constexpr float kArrowGap = 20.0f;
    constexpr float kAddWidth = 30.0f;
    constexpr float kDragThreshold = 4.0f;

    void drawArrow (juce::Graphics& g, juce::Point<float> c, juce::Colour colour)
    {
        juce::Path p;
        p.startNewSubPath (c.x - 4.0f, c.y);
        p.lineTo (c.x + 2.0f, c.y);
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (1.4f));
        juce::Path head;
        head.addTriangle (c.x + 1.0f, c.y - 3.5f, c.x + 5.5f, c.y, c.x + 1.0f, c.y + 3.5f);
        g.fillPath (head);
    }
}

FxChainStrip::FxChainStrip()
{
    setTooltip ("The effects chain, in signal order. Drag a module to move it, click its light to switch it,\n"
                "x or right-click to take it out, + to add one. The output filter, gain and output stage come after it.");
}

juce::Colour FxChainStrip::colourFor (int module)
{
    switch (fxKindOf (module))
    {
        case fxKindDrive:  return colours::drive;
        case fxKindComp:   return juce::Colour (0xffe58cff);
        case fxKindClip:   return juce::Colour (0xffff4f6d);
        case fxKindReverb: return colours::reverb;
        case fxKindEq:
        default:           return colours::eq;
    }
}

void FxChainStrip::setSelected (int module)
{
    if (module != selected)
    {
        selected = module;
        repaint();
    }
}

void FxChainStrip::refresh()
{
    const auto chain = getChain ? getChain() : FxChain {};
    uint32_t bits = 0;
    juce::String readouts;
    for (int m = 0; m < numFxModules; ++m)
    {
        if (isOn && isOn (m))
            bits |= 1u << m;
        if (readout && chain.contains (m))
            readouts << readout (m) << "|";
    }
    if (chain != lastChain || bits != lastOnBits || readouts != lastReadouts)
    {
        lastChain = chain;
        lastOnBits = bits;
        lastReadouts = readouts;
        repaint();
    }
}

FxChain FxChainStrip::shownChain() const
{
    auto chain = getChain ? getChain() : FxChain {};
    if (dragging && dragModule >= 0 && dragTarget >= 0)
        chain.move (dragModule, dragTarget);
    return chain;
}

std::vector<FxChainStrip::Chip> FxChainStrip::layoutChips (const FxChain& chain) const
{
    std::vector<Chip> chips;
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f, 3.0f);
    float x = bounds.getX();
    for (int i = 0; i < chain.length; ++i)
    {
        Chip c;
        c.module = chain[i];
        c.box = { x, bounds.getY(), kChipWidth, bounds.getHeight() };
        c.light = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ c.box.getX() + 12.0f, c.box.getCentreY() });
        c.close = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ c.box.getRight() - 11.0f, c.box.getCentreY() });
        chips.push_back (c);
        x += kChipWidth + kArrowGap;
    }
    return chips;
}

juce::Rectangle<int> FxChainStrip::getChipBounds (int module) const
{
    for (const auto& c : layoutChips (shownChain()))
        if (c.module == module)
            return c.box.toNearestInt();
    return {};
}

juce::Rectangle<int> FxChainStrip::getAddButtonBounds() const
{
    const auto chain = shownChain();
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f, 3.0f);
    const float x = bounds.getX() + (float) chain.length * (kChipWidth + kArrowGap);
    return juce::Rectangle<float> (x, bounds.getY(), kAddWidth, bounds.getHeight()).toNearestInt();
}

FxChainStrip::HitPart FxChainStrip::findPart (juce::Point<float> p) const
{
    if (getAddButtonBounds().toFloat().contains (p))
        return { Part::add, -1 };
    for (const auto& c : layoutChips (shownChain()))
    {
        if (! c.box.contains (p))
            continue;
        if (c.light.expanded (3.0f).contains (p))
            return { Part::light, c.module };
        if (c.close.expanded (2.0f).contains (p))
            return { Part::close, c.module };
        return { Part::chip, c.module };
    }
    return {};
}

void FxChainStrip::paint (juce::Graphics& g)
{
    const auto chain = shownChain();
    const auto chips = layoutChips (chain);

    for (size_t i = 0; i < chips.size(); ++i)
    {
        const auto& c = chips[i];
        if (dragging && c.module == dragModule)
            continue; // drawn last, under the mouse
        if (i + 1 <= chips.size())
            drawArrow (g, { c.box.getRight() + kArrowGap * 0.5f - 1.0f, c.box.getCentreY() }, colours::textDim.withAlpha (0.7f));
    }

    const auto drawChip = [&] (const Chip& c, juce::Rectangle<float> box)
    {
        const auto colour = colourFor (c.module);
        const bool on = isOn ? isOn (c.module) : true;
        const bool sel = c.module == selected;
        const bool hot = hover.module == c.module;
        const auto light = c.light.withCentre ({ box.getX() + 12.0f, box.getCentreY() });
        const auto close = c.close.withCentre ({ box.getRight() - 11.0f, box.getCentreY() });

        g.setColour (sel ? colour.withAlpha (0.18f) : colours::panel);
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (sel ? colour.withAlpha (0.85f) : hot ? colours::gridStrong.brighter (0.3f) : colours::panelBorder);
        g.drawRoundedRectangle (box.reduced (0.5f), 5.0f, sel ? 1.4f : 1.0f);

        // The light: lit in the module's colour while it's on.
        g.setColour (on ? colour : colours::track);
        g.fillEllipse (light.reduced (2.5f));
        if (hover.part == Part::light && hover.module == c.module)
        {
            g.setColour (colours::text.withAlpha (0.6f));
            g.drawEllipse (light.reduced (1.0f), 1.0f);
        }

        const auto textArea = box.withTrimmedLeft (24.0f).withTrimmedRight (hot ? 22.0f : 6.0f);
        const auto extra = readout ? readout (c.module) : juce::String();
        g.setColour (on ? (sel ? colours::text : colours::text.withAlpha (0.85f)) : colours::textDim);
        g.setFont (uiFont (10.5f, true));
        if (extra.isEmpty())
            g.drawText (fxModuleName (c.module), textArea, juce::Justification::centredLeft, true);
        else
        {
            g.drawText (fxModuleName (c.module), textArea.withTrimmedBottom (box.getHeight() * 0.42f), juce::Justification::bottomLeft, true);
            g.setFont (uiFont (9.0f));
            g.setColour (colour.withAlpha (on ? 0.9f : 0.4f));
            g.drawText (extra, textArea.withTrimmedTop (box.getHeight() * 0.55f), juce::Justification::topLeft, true);
        }

        if (hot && ! dragging)
        {
            const bool closeHot = hover.part == Part::close;
            g.setColour (closeHot ? colours::text : colours::textDim);
            const auto x = close.reduced (4.0f);
            g.drawLine ({ x.getTopLeft(), x.getBottomRight() }, 1.3f);
            g.drawLine ({ x.getBottomLeft(), x.getTopRight() }, 1.3f);
        }
    };

    for (const auto& c : chips)
        if (! (dragging && c.module == dragModule))
            drawChip (c, c.box);

    // + (when there's anything left to add), then where the chain goes.
    const auto add = getAddButtonBounds().toFloat();
    const bool full = chain.length >= numFxModules;
    if (! full)
    {
        const bool hot = hover.part == Part::add;
        g.setColour (hot ? colours::gridStrong : colours::panel);
        g.fillRoundedRectangle (add, 5.0f);
        g.setColour (colours::panelBorder);
        g.drawRoundedRectangle (add.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (hot ? colours::text : colours::textDim);
        g.setFont (uiFont (15.0f, true));
        g.drawText ("+", add, juce::Justification::centred);
    }
    const float tail = (full ? add.getX() : add.getRight()) + 8.0f;
    if (tail + 90.0f < (float) getWidth())
    {
        drawArrow (g, { tail + 4.0f, add.getCentreY() }, colours::textDim.withAlpha (0.7f));
        g.setColour (colours::textDim);
        g.setFont (uiFont (10.0f, true));
        g.drawText ("OUTPUT", juce::Rectangle<float> (tail + 14.0f, add.getY(), 80.0f, add.getHeight()),
                    juce::Justification::centredLeft, true);
    }

    if (dragging && dragModule >= 0)
        for (const auto& c : chips)
            if (c.module == dragModule)
            {
                const auto box = c.box.withX (dragX);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.fillRoundedRectangle (box.translated (0.0f, 2.0f), 5.0f);
                drawChip (c, box);
            }
}

void FxChainStrip::mouseMove (const juce::MouseEvent& e)
{
    const auto h = findPart (e.position);
    if (h.part != hover.part || h.module != hover.module)
    {
        hover = h;
        setMouseCursor (h.part == Part::chip ? juce::MouseCursor::DraggingHandCursor
                        : h.part == Part::none ? juce::MouseCursor::NormalCursor
                                               : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void FxChainStrip::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void FxChainStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto h = findPart (e.position);
    dragging = false;
    dragModule = -1;

    if (h.part == Part::add)
    {
        showAddMenu();
        return;
    }
    if (h.module < 0)
        return;

    if (e.mods.isPopupMenu() || h.part == Part::close)
    {
        if (setChain)
        {
            auto chain = getChain();
            chain.remove (h.module);
            setChain (chain);
            if (selected == h.module && onSelect)
                onSelect (chain.length > 0 ? chain[0] : -1);
        }
        hover = {};
        repaint();
        return;
    }
    if (h.part == Part::light)
    {
        if (toggleOn)
            toggleOn (h.module);
        repaint();
        return;
    }

    dragModule = h.module;
    const auto box = getChipBounds (h.module).toFloat();
    dragOffset = e.position.x - box.getX();
    dragX = box.getX();
    dragTarget = getChain().indexOf (h.module);
}

void FxChainStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragModule < 0)
        return;
    if (! dragging && (float) std::abs (e.getDistanceFromDragStartX()) < kDragThreshold)
        return;

    dragging = true;
    const auto chain = getChain();
    const float step = kChipWidth + kArrowGap;
    const float first = getLocalBounds().toFloat().reduced (2.0f, 3.0f).getX();
    dragX = juce::jlimit (first, first + (float) (chain.length - 1) * step, e.position.x - dragOffset);
    dragTarget = juce::jlimit (0, chain.length - 1, juce::roundToInt ((dragX - first) / step));
    repaint();
}

void FxChainStrip::mouseUp (const juce::MouseEvent&)
{
    if (dragModule < 0)
        return;

    const int module = dragModule;
    if (dragging)
    {
        auto chain = getChain();
        if (chain.indexOf (module) != dragTarget && setChain)
        {
            chain.move (module, dragTarget);
            setChain (chain);
        }
    }
    dragging = false;
    dragModule = dragTarget = -1;
    if (onSelect)
        onSelect (module);
    setSelected (module);
    repaint();
}

void FxChainStrip::showAddMenu()
{
    const auto chain = getChain ? getChain() : FxChain {};
    juce::PopupMenu menu;
    menu.addSectionHeader ("ADD TO THE CHAIN");
    for (int m = 0; m < numFxModules; ++m)
        menu.addItem (m + 1, fxModuleName (m), ! chain.contains (m));

    juce::Component::SafePointer<FxChainStrip> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (
                            localAreaToGlobal (getAddButtonBounds())),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result <= 0 || ! safe->setChain)
                                return;
                            auto c = safe->getChain();
                            // After the selected module, or at the end.
                            const int at = safe->selected >= 0 && c.contains (safe->selected) ? c.indexOf (safe->selected) + 1 : -1;
                            if (c.add (result - 1, at))
                            {
                                safe->setChain (c);
                                safe->setSelected (result - 1);
                                if (safe->onSelect)
                                    safe->onSelect (result - 1);
                            }
                        });
}

} // namespace drum::ui
