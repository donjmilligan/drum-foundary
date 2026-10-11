// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"

namespace drum::ui
{

/** The display's LFO tab: LFO 1, 2 and 3 side by side. Each has its shape, its
    mode (Retrig, One Shot or Free, see LfoBank), its Rate or a tempo Sync, the
    Phase a hit starts it at, a picture of what it does over one hit (Length) with
    a live marker, and its target slots.

    Drag an LFO's handle (its name) onto any knob to modulate that knob's parameter.
    The slot list shows what each LFO modulates and how far (Amount: -100% to +100%
    of the target's range, from its knob's position); x clears a slot. Clicking a
    slot's name picks its target from a menu instead. */
class LfoPage : public juce::Component, private juce::Timer
{
public:
    explicit LfoPage (DrumFoundryProcessor& processor);
    ~LfoPage() override;

    /** LFO `index`'s drag handle (for tests). */
    juce::Component& getHandle (int index);

    void resized() override;
    void visibilityChanged() override;

private:
    class Handle;
    class Preview;
    class SlotRow;
    class Panel;

    void timerCallback() override;

    std::array<std::unique_ptr<Panel>, kNumLfos> panels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoPage)
};

} // namespace drum::ui
