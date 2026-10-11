// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <optional>

namespace drum::ui
{

/** What a keyboard shortcut does. */
enum class KeyAction
{
    play,
    undo,
    redo,
    previousPreset,
    nextPreset,
    toggleBrowser,
    numActions
};

/** The keyboard shortcuts, up to two keys per action. Set in the settings, kept in
    Drum Foundry/Keys.settings, and shared by every open instance (through a
    juce::SharedResourcePointer), which hear about changes as a change message.
    Message thread only. */
class KeyMap : public juce::ChangeBroadcaster
{
public:
    static constexpr int numActions = (int) KeyAction::numActions;
    static constexpr int keysPerAction = 2;

    KeyMap();

    /** "Play hit", "Undo" ... */
    static juce::String actionName (KeyAction);
    /** "Space", "Cmd+Shift+Z" (Ctrl on Windows), "Left"; empty for no key. */
    static juce::String describe (const juce::KeyPress&);

    /** The key in `slot` (0 or 1) for `action`; an invalid KeyPress if there's none. */
    juce::KeyPress getKey (KeyAction, int slot) const;
    /** Sets a key; an invalid KeyPress clears the slot. A key another slot already
        has moves here (the other slot is cleared). */
    void setKey (KeyAction, int slot, const juce::KeyPress&);
    void resetToDefaults();

    /** The action `key` is set to, if any. */
    std::optional<KeyAction> find (const juce::KeyPress&) const;
    /** The keys set for `action`, as text for tooltips ("Space", "Cmd+Z or Cmd+Y"). */
    juce::String describeKeys (KeyAction) const;

private:
    using Keys = std::array<std::array<juce::KeyPress, keysPerAction>, numActions>;
    static Keys defaults();
    void save();

    Keys keys;
    juce::PropertiesFile file;
};

} // namespace drum::ui
