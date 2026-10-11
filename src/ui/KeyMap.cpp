// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "KeyMap.h"

namespace drum::ui
{

namespace
{
    // Ids in Keys.settings: change one and that action's saved keys are lost.
    const char* actionId (KeyAction action)
    {
        switch (action)
        {
            case KeyAction::play:           return "play";
            case KeyAction::undo:           return "undo";
            case KeyAction::redo:           return "redo";
            case KeyAction::previousPreset: return "previousPreset";
            case KeyAction::nextPreset:     return "nextPreset";
            case KeyAction::toggleBrowser:  return "toggleBrowser";
            case KeyAction::numActions:     break;
        }
        return "";
    }

    juce::String settingName (int action, int slot)
    {
        return juce::String (actionId ((KeyAction) action)) + "." + juce::String (slot + 1);
    }

    // A slot the user cleared (a missing one means the default).
    constexpr const char* noKey = "none";

    // Only the key and its modifiers count: what it types and the mouse buttons don't.
    juce::KeyPress normalised (const juce::KeyPress& key)
    {
        if (! key.isValid())
            return {};
        return juce::KeyPress (key.getKeyCode(), key.getModifiers().withoutMouseButtons(), 0);
    }

    juce::PropertiesFile::Options fileOptions()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Keys";
        o.folderName = "Drum Foundry";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.millisecondsBeforeSaving = 500;
        return o;
    }
}

KeyMap::KeyMap()
    : keys (defaults()), file (fileOptions())
{
    for (int a = 0; a < numActions; ++a)
        for (int s = 0; s < keysPerAction; ++s)
            if (const auto saved = file.getValue (settingName (a, s)); saved.isNotEmpty())
                keys[(size_t) a][(size_t) s] = saved == noKey ? juce::KeyPress() : normalised (juce::KeyPress::createFromDescription (saved));
}

KeyMap::Keys KeyMap::defaults()
{
    const auto cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
    Keys k;
    k[(size_t) KeyAction::play]  = { juce::KeyPress (juce::KeyPress::spaceKey), {} };
    k[(size_t) KeyAction::undo]  = { juce::KeyPress ('Z', cmd, 0), {} };
    k[(size_t) KeyAction::redo]  = { juce::KeyPress ('Z', cmd | shift, 0), juce::KeyPress ('Y', cmd, 0) };
    // Previous / next preset and the browser start without keys: the arrows and letters
    // already mean something to the menus and the browser when they have the focus.
    return k;
}

juce::String KeyMap::actionName (KeyAction action)
{
    switch (action)
    {
        case KeyAction::play:           return "Play hit";
        case KeyAction::undo:           return "Undo";
        case KeyAction::redo:           return "Redo";
        case KeyAction::previousPreset: return "Previous preset";
        case KeyAction::nextPreset:     return "Next preset";
        case KeyAction::toggleBrowser:  return "Show / hide browser";
        case KeyAction::numActions:     break;
    }
    return {};
}

juce::String KeyMap::describe (const juce::KeyPress& key)
{
    if (! key.isValid())
        return {};

    const auto mods = key.getModifiers();
    juce::StringArray parts;
   #if JUCE_MAC
    if (mods.isCommandDown()) parts.add ("Cmd");
    if (mods.isCtrlDown())    parts.add ("Ctrl");
    if (mods.isAltDown())     parts.add ("Opt");
   #else
    if (mods.isCtrlDown())    parts.add ("Ctrl");
    if (mods.isAltDown())     parts.add ("Alt");
   #endif
    if (mods.isShiftDown())   parts.add ("Shift");

    // JUCE's name for the bare key ("spacebar", "cursor left", "F5", "Z"), tidied.
    auto name = juce::KeyPress (key.getKeyCode()).getTextDescription();
    if (name == "spacebar")
        name = "Space";
    name = name.replace ("cursor ", "");
    parts.add (name.substring (0, 1).toUpperCase() + name.substring (1));
    return parts.joinIntoString ("+");
}

juce::KeyPress KeyMap::getKey (KeyAction action, int slot) const
{
    return keys[(size_t) action][(size_t) slot];
}

void KeyMap::setKey (KeyAction action, int slot, const juce::KeyPress& key)
{
    const auto k = normalised (key);
    if (k.isValid())
        for (auto& slots : keys)
            for (auto& other : slots)
                if (other == k)
                    other = {};

    keys[(size_t) action][(size_t) slot] = k;
    save();
}

void KeyMap::resetToDefaults()
{
    keys = defaults();
    file.clear();
    file.saveIfNeeded();
    sendChangeMessage();
}

std::optional<KeyAction> KeyMap::find (const juce::KeyPress& key) const
{
    const auto k = normalised (key);
    if (! k.isValid())
        return std::nullopt;

    for (int a = 0; a < numActions; ++a)
        for (const auto& slot : keys[(size_t) a])
            if (slot.isValid() && slot == k)
                return (KeyAction) a;
    return std::nullopt;
}

juce::String KeyMap::describeKeys (KeyAction action) const
{
    juce::StringArray names;
    for (const auto& slot : keys[(size_t) action])
        if (slot.isValid())
            names.add (describe (slot));
    return names.joinIntoString (" or ");
}

void KeyMap::save()
{
    for (int a = 0; a < numActions; ++a)
        for (int s = 0; s < keysPerAction; ++s)
        {
            const auto& k = keys[(size_t) a][(size_t) s];
            file.setValue (settingName (a, s), k.isValid() ? k.getTextDescription() : juce::String (noKey));
        }
    sendChangeMessage();
}

} // namespace drum::ui
