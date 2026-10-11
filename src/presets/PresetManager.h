// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class DrumFoundryProcessor;

/** Factory presets (compiled in) followed by user presets (XML files in
    ~/Music/Drum Foundry/Presets). All methods are message-thread only.

    getEntries() keeps that order (factory presets in their compiled order, then user
    presets by name), so an entry's index stays put. They're shown and stepped through
    grouped instead (getDisplayOrder()): factory kicks, factory snares, then the user's
    kicks and snares, each group with its Init first and the rest A to Z.

    A user preset stores the absolute paths of its imported samples (see
    DrumFoundryProcessor::createStateTree()); a file that has gone since shows
    as missing. Factory presets have no samples, so loading one empties every
    sample slot. */
class PresetManager
{
public:
    enum class Category { kick, snare };

    struct Entry
    {
        juce::String name;
        bool isFactory = true;
        int factoryIndex = -1;
        juce::File file;
        /** A kick if its name says so, or (a user preset) its Body Style is one of the kick
            bodies (Kick, Round, Taper, Hard, Fold, Tonal); otherwise a snare. */
        Category category = Category::snare;
    };

    explicit PresetManager (DrumFoundryProcessor& processor);

    void refresh();
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    /** Indices into getEntries() in the order they're listed and stepped through. */
    const std::vector<int>& getDisplayOrder() const noexcept { return displayOrder; }

    /** Index into getEntries() matching the current patch name, or -1. */
    int getCurrentIndex() const;
    juce::String getCurrentName() const;

    void load (int index);
    void loadNext()     { step (1); }
    void loadPrevious() { step (-1); }

    /** Saves the current patch as a user preset. Returns false on failure. */
    bool saveUserPreset (const juce::String& name);

    static juce::File getUserFolder();
    static constexpr const char* fileExtension = ".drumpreset";

private:
    void step (int delta);
    juce::ValueTree buildFactoryState (int factoryIndex) const;

    DrumFoundryProcessor& processor;
    std::vector<Entry> entries;
    std::vector<int> displayOrder;
};
