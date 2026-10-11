// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "PluginProcessor.h"
#include "ui/FoundryLookAndFeel.h"
#include "ui/KeyMap.h"

class DrumFoundryEditor : public juce::AudioProcessorEditor,
                       private juce::Timer,
                       private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int baseWidth = 1440;
    static constexpr int baseHeight = 880;

    explicit DrumFoundryEditor (DrumFoundryProcessor&);
    ~DrumFoundryEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Re-renders the waveform display immediately (for the last note played, with
        KEY on), on the calling thread (message thread only). Normal edits render in the background instead; this is for the
        first frame and for tools that need a finished display right away. */
    void refreshWaveform();

    /** Shows or hides the sample browser (the header's BROWSER button). */
    void setBrowserVisible (bool shouldBeVisible);
    bool isBrowserVisible() const;
    /** Shows or hides the settings (the header's gear): the sidechain trigger and window size. */
    void setSettingsVisible (bool shouldBeVisible);
    bool isSettingsVisible() const;
    /** Shows or hides the full-screen EQ (the EQ section's FULL button). */
    void setEqVisible (bool shouldBeVisible);
    bool isEqVisible() const;
    /** Shows or hides the full-screen reverb (the Reverb section's FULL button). */
    void setReverbVisible (bool shouldBeVisible);
    bool isReverbVisible() const;
    /** Opens the browser pointed at `slot`, as that slot's BROWSE button does. */
    void browseSlot (drum::SampleSlot slot);
    /** Shows extra layer `index`'s controls (0-based), as clicking its tab does. */
    void selectExtraLayer (int index);
    /** Shows a tab of the Layers section (see LayersSection: Noise, Click, extra layers). */
    void selectLayerTab (int tab);
    /** Adds an extra layer playing `source` (an index into layerSourceNames), as the
        Layers section's "+" menu does. Returns its index, or -1 if none is free. */
    int addExtraLayer (int source);
    /** What pressing a layer does: -1 = the Body panel, otherwise a Layers tab (as
        clicking it). The display then shows that layer's curves. */
    void touchLayer (int tab);

    /** Undo / redo the last patch change (the header's buttons, Cmd+Z, Cmd+Shift+Z / Cmd+Y),
        and bring the display, tabs and header up to date at once. */
    void undo();
    void redo();
    /** The keyboard shortcuts (the settings' key map: play, undo, redo, presets, browser).
        Keys only get here while something in the editor has the keyboard focus (a
        button, menu or the browser: the plugin doesn't take the focus from the host by
        itself), and aren't used by it. */
    bool keyPressed (const juce::KeyPress&) override;
    /** Notices the play key going up, so holding it plays one hit, not a roll. */
    bool keyStateChanged (bool isKeyDown) override;

private:
    class MainPanel;
    class WaveformRenderer;
    class ActivityWatcher;

    void timerCallback() override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void pollChanges();
    /** Follows the last note played (with KEY on) in the render, the ruler and the badge. */
    void updateDisplayNote();
    void showNewWaveform();
    void updateGateMarker();
    /** Lets the undo history commit or absorb changes (timer, and every mouse-up). */
    void pollUndo();
    void updateHistoryButtons();
    juce::File exportWav();

    DrumFoundryProcessor& drumProcessor;
    drum::ui::FoundryLookAndFeel lookAndFeel;
    std::unique_ptr<MainPanel> main;
    juce::TooltipWindow tooltips { this, 700 };

    std::atomic<bool> waveformDirty { false }; // the constructor renders the first frame itself
    int lastStateVersion = -1;
    int lastSidechainHits = 0;
    float sidechainGlow = 0.0f;
    float rulerTune = 0.0f;
    std::unique_ptr<ActivityWatcher> activityWatcher;
    int lastHistoryRevision = -1;
    int undoPollTicks = 0;

    juce::SharedResourcePointer<drum::ui::KeyMap> keyMap;
    int heldPlayKey = 0;        // the play key's code while it's held down (its repeats are ignored)
    double heldPlayKeyTime = 0.0;

    // Last, so it's destroyed first (the destructor also stops it explicitly).
    std::unique_ptr<WaveformRenderer> renderer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumFoundryEditor)
};
