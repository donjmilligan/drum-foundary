// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../dsp/FxChain.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace drum::ui
{

/** The master chain as a row of chips in signal order, ending in the fixed output.
    Click a chip to show its controls, drag it left or right to move it, click its
    light to switch it on or off, and its x (or a right-click) to take it out of the
    chain. + adds any module that isn't in it yet. A drag is one change, made when
    the mouse is released; the chips slide to show where it will land meanwhile.

    It owns no state: it reads and writes through the callbacks, so the editor
    decides where the chain lives (the processor) and what a module's switch is (a
    parameter). Call refresh() from a timer; it repaints only when something moved. */
class FxChainStrip : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    std::function<FxChain()> getChain;
    std::function<void (const FxChain&)> setChain;
    std::function<bool (int module)> isOn;
    std::function<void (int module)> toggleOn;
    /** A module was clicked (or added): show its controls. */
    std::function<void (int module)> onSelect;
    /** Optional: a short live readout for a chip (e.g. a compressor's "-4.5 dB"). */
    std::function<juce::String (int module)> readout;

    FxChainStrip();

    void setSelected (int module);
    int getSelected() const noexcept { return selected; }
    void refresh();

    /** The x range of the chip for `module` (for tests), or empty. */
    juce::Rectangle<int> getChipBounds (int module) const;
    juce::Rectangle<int> getAddButtonBounds() const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static juce::Colour colourFor (int module);

private:
    struct Chip
    {
        int module = -1;
        juce::Rectangle<float> box, light, close;
    };

    enum class Part { none, chip, light, close, add };
    struct HitPart { Part part = Part::none; int module = -1; };

    std::vector<Chip> layoutChips (const FxChain& chain) const;
    HitPart findPart (juce::Point<float> p) const;
    void showAddMenu();
    FxChain shownChain() const;

    int selected = -1;
    HitPart hover;
    int dragModule = -1, dragTarget = -1;
    float dragOffset = 0.0f, dragX = 0.0f;
    bool dragging = false;

    // What refresh() last saw, to repaint only on a change.
    FxChain lastChain;
    uint32_t lastOnBits = 0;
    juce::String lastReadouts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxChainStrip)
};

} // namespace drum::ui
