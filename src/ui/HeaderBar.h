// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include "KeyMap.h"

namespace drum::ui
{

/** Round pad that fires a hit when pressed. */
class TriggerPad : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onTrigger;
    /** Fires a hit and lights the pad briefly, as a click does (the play key). */
    void press();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    bool pressed = false;
};

/** Drag this into the DAW to drop the current sound in as a WAV. */
class DragExport : public juce::Component, public juce::SettableTooltipClient
{
public:
    /** Renders the sound to disk and returns the file (or {} on failure). */
    std::function<juce::File()> exportFile;

    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    bool dragging = false;
};

class LevelMeter : public juce::Component
{
public:
    void setLevels (float left, float right);
    void paint (juce::Graphics&) override;

private:
    float levels[2] {};
};

/** Modal-looking overlay for naming a preset. Lives inside the editor so it
    behaves the same in every host. */
class SaveOverlay : public juce::Component
{
public:
    SaveOverlay();

    std::function<void (const juce::String&)> onSave;

    void show (const juce::String& initialName);
    void hide();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> boxBounds() const;
    void submit();

    juce::TextEditor nameEditor;
    juce::TextButton saveButton { "SAVE" }, cancelButton { "CANCEL" };
};

/** One key of a keyboard shortcut in the settings. Click it, then press the key to
    use (Backspace or Delete clears it, Escape keeps the old one). */
class KeyCaptureButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    KeyCaptureButton();

    /** A key was pressed while listening (an invalid KeyPress: cleared). */
    std::function<void (const juce::KeyPress&)> onKeyChosen;
    void setKey (const juce::KeyPress&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    juce::KeyPress key;
    bool listening = false;
};

/** Modal-looking settings page: options that aren't part of the sound, such as the
    sidechain trigger (a component owned elsewhere), the window size and the keyboard
    shortcuts. */
class SettingsOverlay : public juce::Component, private juce::ChangeListener
{
public:
    SettingsOverlay();
    ~SettingsOverlay() override;

    /** The sidechain trigger section to show (it must outlive this). */
    void setTriggerSection (juce::Component& section);
    /** Called when the close button, Escape or a click outside the box asks to close. */
    std::function<void()> onClose;
    /** A window size was chosen (1 = the base size). */
    std::function<void (float)> onScaleChosen;
    /** Lights the size button nearest `scale`. */
    void setCurrentScale (float scale);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    juce::Rectangle<int> boxBounds() const;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateKeyButtons();

    juce::Rectangle<int> helpArea, sizeLabelArea, keysLabelArea, aboutArea;
    std::array<juce::Rectangle<int>, KeyMap::numActions> keyNameAreas;

    juce::Component* triggerSection = nullptr;
    juce::TextButton closeButton;
    static constexpr float scales[] { 0.75f, 1.0f, 1.25f, 1.5f };
    std::array<juce::TextButton, 4> scaleButtons;

    juce::SharedResourcePointer<KeyMap> keyMap;
    std::array<std::array<KeyCaptureButton, KeyMap::keysPerAction>, KeyMap::numActions> keyButtons;
    juce::TextButton resetKeysButton { "RESET" };
    juce::HyperlinkButton siteLink { "therestears.com", juce::URL ("https://therestears.com") };
    juce::HyperlinkButton licenseLink { "License: GNU AGPL v3", juce::URL ("https://www.gnu.org/licenses/agpl-3.0.html") };
};

/** The header's gear: opens the settings. A dot in its corner shows the sidechain
    trigger is on, and flashes on each sidechain hit. */
class GearButton : public juce::Button
{
public:
    GearButton();

    /** Sidechain trigger on (`armed`), and its hit light (0..1). */
    void setSidechainState (bool armed, float activity);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    bool scArmed = false;
    float scActivity = 0.0f;
};

/** The header's undo or redo button: a curved arrow, dimmed while there's nothing
    to undo / redo. */
class HistoryButton : public juce::Button
{
public:
    explicit HistoryButton (bool isRedo);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    const bool redo;
};

class HeaderBar : public juce::Component, private juce::ChangeListener
{
public:
    explicit HeaderBar (DrumFoundryProcessor& processor);
    ~HeaderBar() override;

    /** Rebuilds the preset list (after saving) and updates the selection. */
    void refreshPresets();
    /** Updates the displayed preset name only. */
    void updatePresetDisplay();
    void setLevels (float left, float right) { meter.setLevels (left, right); }

    std::function<void()> onSaveClicked;
    /** The BROWSER button was clicked (it shows the browser's visibility, see setBrowserShown()). */
    std::function<void()> onBrowserClicked;
    void setBrowserShown (bool shown) { browserButton.setToggleState (shown, juce::dontSendNotification); }
    /** The gear was clicked (it shows whether the settings are open, see setSettingsShown()). */
    std::function<void()> onSettingsClicked;
    void setSettingsShown (bool shown) { settingsButton.setToggleState (shown, juce::dontSendNotification); }
    void setSidechainState (bool armed, float activity) { settingsButton.setSidechainState (armed, activity); }

    /** The undo / redo buttons were clicked. */
    std::function<void()> onUndo, onRedo;
    /** Enables the undo / redo buttons and names what they'd revert in their tooltips
        ("Tune", "preset load"; empty = nothing to undo / redo). */
    void setHistoryState (const juce::String& undoWhat, const juce::String& redoWhat);
    HistoryButton& getUndoButton() noexcept { return undoButton; }
    HistoryButton& getRedoButton() noexcept { return redoButton; }
    /** The < and > buttons' job: the previous / next preset (their shortcuts). */
    void showPreviousPreset() { prevButton.triggerClick(); }
    void showNextPreset()     { nextButton.triggerClick(); }

    TriggerPad trigger;
    DragExport dragExport;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    /** Puts the current keys in the tooltips of the buttons that have shortcuts. */
    void updateShortcutTooltips();

    DrumFoundryProcessor& processor;
    juce::SharedResourcePointer<KeyMap> keyMap;
    juce::String undoWhat, redoWhat;
    juce::TextButton prevButton { "<" }, nextButton { ">" }, saveButton { "SAVE" }, browserButton { "BROWSER" };
    GearButton settingsButton;
    HistoryButton undoButton { false }, redoButton { true };
    juce::ComboBox presetBox;
    LevelMeter meter;
    juce::Rectangle<int> logoArea;
};

} // namespace drum::ui
