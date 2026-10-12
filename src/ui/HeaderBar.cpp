// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "HeaderBar.h"
#include "FoundryLookAndFeel.h"

namespace drum::ui
{

//==============================================================================
void TriggerPad::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.5f);
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto circle = bounds.withSizeKeepingCentre (size, size);
    const bool hot = isMouseOver();

    g.setColour (pressed ? colours::body.withAlpha (0.35f) : colours::display);
    g.fillEllipse (circle);
    g.setColour (pressed || hot ? colours::body : colours::panelBorder.brighter (0.2f));
    g.drawEllipse (circle, 1.5f);

    const auto c = circle.getCentre();
    const float r = size * 0.2f;
    juce::Path tri;
    tri.addTriangle (c.x - r * 0.7f, c.y - r, c.x - r * 0.7f, c.y + r, c.x + r * 1.05f, c.y);
    g.setColour (pressed || hot ? colours::body : colours::text);
    g.fillPath (tri);
}

void TriggerPad::mouseDown (const juce::MouseEvent&)
{
    pressed = true;
    repaint();
    if (onTrigger)
        onTrigger();
}

void TriggerPad::mouseUp (const juce::MouseEvent&)
{
    pressed = false;
    repaint();
}

void TriggerPad::press()
{
    pressed = true;
    repaint();
    if (onTrigger)
        onTrigger();
    juce::Timer::callAfterDelay (110, [safe = juce::Component::SafePointer<TriggerPad> (this)]
    {
        if (safe != nullptr)
        {
            safe->pressed = false;
            safe->repaint();
        }
    });
}

//==============================================================================
void DragExport::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool hot = isMouseOver() || dragging;

    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (hot ? colours::body : colours::panelBorder.brighter (0.2f));
    g.drawRoundedRectangle (bounds, 5.0f, 1.2f);

    // Little waveform glyph
    auto icon = bounds.reduced (9.0f, 0.0f).removeFromLeft (16.0f).withSizeKeepingCentre (16.0f, 16.0f);
    static constexpr float bars[] { 0.35f, 1.0f, 0.7f, 0.45f, 0.25f };
    const float bw = icon.getWidth() / (float) std::size (bars);
    g.setColour (hot ? colours::body : colours::text);
    for (size_t i = 0; i < std::size (bars); ++i)
    {
        const float h = icon.getHeight() * bars[i];
        g.fillRoundedRectangle (icon.getX() + (float) i * bw + 0.5f, icon.getCentreY() - h * 0.5f, bw - 1.2f, h, 0.8f);
    }

    g.setFont (uiFont (11.5f, true));
    g.drawText ("DRAG", bounds.withTrimmedLeft (28.0f).withTrimmedRight (6.0f), juce::Justification::centred);
}

void DragExport::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 5 || exportFile == nullptr)
        return;

    const auto file = exportFile();
    if (! file.existsAsFile())
        return;

    dragging = true;
    repaint();
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [safe = juce::Component::SafePointer<DragExport> (this)]
                                                                {
                                                                    if (safe != nullptr)
                                                                    {
                                                                        safe->dragging = false;
                                                                        safe->repaint();
                                                                    }
                                                                });
}

void DragExport::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

//==============================================================================
void LevelMeter::setLevels (float left, float right)
{
    const float in[2] { left, right };
    bool changed = false;
    for (int c = 0; c < 2; ++c)
    {
        // Instant attack, ~300 ms release at 30 fps.
        const float next = juce::jmax (in[c], levels[c] * 0.82f);
        if (std::abs (next - levels[c]) > 1.0e-4f)
        {
            levels[c] = next;
            changed = true;
        }
    }
    if (changed)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const float barW = (bounds.getWidth() - 3.0f) * 0.5f;

    for (int c = 0; c < 2; ++c)
    {
        const auto bar = juce::Rectangle<float> (bounds.getX() + (float) c * (barW + 3.0f), bounds.getY(), barW, bounds.getHeight());
        g.setColour (colours::display);
        g.fillRoundedRectangle (bar, 2.0f);

        const float db = juce::Decibels::gainToDecibels (levels[c], -48.0f);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
        if (norm > 0.0f)
        {
            const auto fill = bar.withTop (bar.getBottom() - bar.getHeight() * norm);
            g.setGradientFill (juce::ColourGradient (colours::eq, 0.0f, bar.getBottom(),
                                                     colours::drive, 0.0f, bar.getY(), false));
            g.fillRoundedRectangle (fill, 2.0f);
        }
    }
}

//==============================================================================
SaveOverlay::SaveOverlay()
{
    nameEditor.setFont (uiFont (15.0f));
    nameEditor.setIndents (8, 6);
    nameEditor.onReturnKey = [this] { submit(); };
    nameEditor.onEscapeKey = [this] { hide(); };
    addAndMakeVisible (nameEditor);

    saveButton.setToggleState (true, juce::dontSendNotification);
    saveButton.setColour (juce::TextButton::buttonOnColourId, colours::body);
    saveButton.onClick = [this] { submit(); };
    addAndMakeVisible (saveButton);

    cancelButton.onClick = [this] { hide(); };
    addAndMakeVisible (cancelButton);

    setVisible (false);
}

void SaveOverlay::show (const juce::String& initialName)
{
    nameEditor.setText (initialName, false);
    nameEditor.selectAll();
    setVisible (true);
    toFront (true);
    nameEditor.grabKeyboardFocus();
}

void SaveOverlay::hide()
{
    setVisible (false);
}

void SaveOverlay::submit()
{
    const auto name = nameEditor.getText().trim();
    if (name.isEmpty())
        return;

    hide();
    if (onSave)
        onSave (name);
}

juce::Rectangle<int> SaveOverlay::boxBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (360, 150);
}

void SaveOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));

    const auto box = boxBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

    g.setFont (titleFont (13.0f));
    g.setColour (colours::text);
    g.drawText ("SAVE PRESET", box.reduced (18.0f, 14.0f).removeFromTop (18.0f), juce::Justification::centredLeft);
}

void SaveOverlay::resized()
{
    auto box = boxBounds().reduced (18, 14);
    box.removeFromTop (26);
    nameEditor.setBounds (box.removeFromTop (34));
    box.removeFromTop (16);
    auto buttons = box.removeFromTop (30);
    saveButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
}

void SaveOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! boxBounds().contains (e.getPosition()))
        hide();
}

//==============================================================================
KeyCaptureButton::KeyCaptureButton()
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Click, then press a key. Backspace clears it, Esc keeps it.");
}

void KeyCaptureButton::setKey (const juce::KeyPress& newKey)
{
    key = newKey;
    repaint();
}

void KeyCaptureButton::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (listening ? colours::body.withAlpha (0.16f) : colours::display);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (listening ? colours::body : (isMouseOver() ? colours::textDim : colours::panelBorder));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    const auto text = listening ? juce::String ("Press a key") : KeyMap::describe (key);
    g.setFont (uiFont (11.5f, ! listening && text.isNotEmpty()));
    g.setColour (listening ? colours::body : (text.isEmpty() ? colours::textDim.withAlpha (0.6f) : colours::text));
    g.drawFittedText (text.isEmpty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) : text,
                      getLocalBounds().reduced (6, 0), juce::Justification::centred, 1);
}

void KeyCaptureButton::mouseDown (const juce::MouseEvent&)
{
    listening = ! listening;
    if (listening)
        grabKeyboardFocus();
    repaint();
}

bool KeyCaptureButton::keyPressed (const juce::KeyPress& pressed)
{
    if (! listening)
        return false;

    listening = false;
    repaint();
    if (pressed == juce::KeyPress::escapeKey)
        return true;

    const bool clear = pressed == juce::KeyPress::backspaceKey || pressed == juce::KeyPress::deleteKey;
    if (onKeyChosen)
        onKeyChosen (clear ? juce::KeyPress() : pressed);
    return true;
}

void KeyCaptureButton::focusLost (FocusChangeType)
{
    listening = false;
    repaint();
}

//==============================================================================
SettingsOverlay::SettingsOverlay()
{
    closeButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
    closeButton.setTooltip ("Close the settings");
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    for (size_t i = 0; i < scaleButtons.size(); ++i)
    {
        auto& b = scaleButtons[i];
        b.setButtonText (juce::String (juce::roundToInt (scales[i] * 100.0f)) + "%");
        b.setColour (juce::TextButton::buttonOnColourId, colours::body);
        b.setTooltip ("Window size: " + b.getButtonText() + " (or drag the corner)");
        b.onClick = [this, i] { if (onScaleChosen) onScaleChosen (scales[i]); };
        addAndMakeVisible (b);
    }

    for (int a = 0; a < KeyMap::numActions; ++a)
        for (int slot = 0; slot < KeyMap::keysPerAction; ++slot)
        {
            auto& b = keyButtons[(size_t) a][(size_t) slot];
            b.onKeyChosen = [this, a, slot] (const juce::KeyPress& key) { keyMap->setKey ((KeyAction) a, slot, key); };
            addAndMakeVisible (b);
        }
    resetKeysButton.setTooltip ("Put back the standard keyboard shortcuts");
    resetKeysButton.onClick = [this] { keyMap->resetToDefaults(); };
    addAndMakeVisible (resetKeysButton);
    keyMap->addChangeListener (this);
    updateKeyButtons();

    for (auto* link : { &siteLink, &licenseLink })
    {
        link->setColour (juce::HyperlinkButton::textColourId, colours::text.withAlpha (0.8f));
        addAndMakeVisible (link);
    }
    siteLink.setFont (uiFont (12.0f), false, juce::Justification::centredLeft);
    licenseLink.setFont (uiFont (12.0f), false, juce::Justification::centredRight);
    siteLink.setTooltip ("Theres Tears Records: https://therestears.com");
    licenseLink.setTooltip ("The licence Drum Foundry is under (its added terms are in the NOTICE file)");

    setWantsKeyboardFocus (true);
    setVisible (false);
}

SettingsOverlay::~SettingsOverlay()
{
    keyMap->removeChangeListener (this);
}

void SettingsOverlay::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateKeyButtons();
}

void SettingsOverlay::updateKeyButtons()
{
    for (int a = 0; a < KeyMap::numActions; ++a)
        for (int slot = 0; slot < KeyMap::keysPerAction; ++slot)
            keyButtons[(size_t) a][(size_t) slot].setKey (keyMap->getKey ((KeyAction) a, slot));
}

void SettingsOverlay::setTriggerSection (juce::Component& section)
{
    triggerSection = &section;
    addAndMakeVisible (section);
    resized();
}

void SettingsOverlay::setCurrentScale (float scale)
{
    size_t nearest = 0;
    for (size_t i = 1; i < std::size (scales); ++i)
        if (std::abs (scales[i] - scale) < std::abs (scales[nearest] - scale))
            nearest = i;
    for (size_t i = 0; i < scaleButtons.size(); ++i)
        scaleButtons[i].setToggleState (i == nearest && std::abs (scales[i] - scale) < 0.06f, juce::dontSendNotification);
}

juce::Rectangle<int> SettingsOverlay::boxBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (500, 672);
}

void SettingsOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));

    const auto box = boxBounds().toFloat();
    g.setColour (colours::panel.brighter (0.03f));
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

    g.setFont (titleFont (13.0f));
    g.setColour (colours::text);
    g.drawText ("SETTINGS", box.reduced (18.0f, 14.0f).removeFromTop (20.0f), juce::Justification::centredLeft);

    g.setFont (uiFont (11.5f));
    g.setColour (colours::textDim);
    g.drawFittedText ("Audio on Drum Foundry's sidechain input fires a hit on C3 at every transient over Threshold "
                      "(velocity from how far it peaks above). It's only listened to, never output.\n"
                      "Ableton Live: use the VST3, open the device's Sidechain section and choose the source under Audio From.\n"
                      "Sidechain settings belong to the session: loading a preset leaves them alone.",
                      helpArea, juce::Justification::topLeft, 6, 1.0f);

    g.setFont (uiFont (11.5f, true));
    g.setColour (colours::text);
    g.drawText ("WINDOW SIZE", sizeLabelArea, juce::Justification::centredLeft);

    // Keyboard shortcuts: work while something in the plug-in has the keyboard focus.
    g.drawText ("KEYBOARD SHORTCUTS", keysLabelArea, juce::Justification::centredLeft);
    g.setFont (uiFont (11.5f));
    g.setColour (colours::textDim);
    g.drawText ("after a click in the plug-in", keysLabelArea.withTrimmedLeft (150), juce::Justification::centredLeft);
    g.setFont (uiFont (12.0f));
    g.setColour (colours::text);
    for (int a = 0; a < KeyMap::numActions; ++a)
        g.drawText (KeyMap::actionName ((KeyAction) a), keyNameAreas[(size_t) a], juce::Justification::centredLeft);

    // About
    g.setColour (colours::panelBorder);
    g.drawHorizontalLine (aboutArea.getY() - 12, (float) aboutArea.getX(), (float) aboutArea.getRight());
    auto about = aboutArea;
    g.setFont (titleFont (14.0f));
    g.setColour (colours::text);
    g.drawText ("Drum Foundry " + juce::String (DRUMFOUNDRY_VERSION), about.removeFromTop (about.getHeight() / 2),
                juce::Justification::bottomLeft);
    g.setFont (uiFont (12.0f));
    g.setColour (colours::textDim);
    auto by = about.withTrimmedTop (4).withHeight (juce::jmin (about.getHeight() - 4, 18));
    g.drawText ("by", by, juce::Justification::centredLeft);
    by.removeFromLeft ((int) juce::GlyphArrangement::getStringWidth (uiFont (12.0f), "by") + 6);
    drawLabelWordmark (g, by.toFloat(), 0.9f);

    // The legal notices: ours (the AGPL's Appropriate Legal Notices, which forks must keep, see
    // NOTICE) and the third-party code in the plug-in (its licence asks for this notice).
    g.setFont (uiFont (10.5f));
    g.setColour (colours::textDim.withAlpha (0.85f));
    g.drawFittedText ("(c) 2026 Don Milligan. Free software under the\n"
                      "GNU AGPL v3 with added terms (NOTICE). No warranty.\n"
                      "Built with JUCE. Reverb: Cloud Seed (c) 2024\n"
                      "Ghost Note Audio, used under its MIT License.",
                      aboutArea.withTrimmedLeft (aboutArea.getWidth() * 2 / 5), juce::Justification::centredRight, 4);
}

void SettingsOverlay::resized()
{
    auto box = boxBounds().reduced (18, 14);
    closeButton.setBounds (box.getRight() - 24, box.getY() - 2, 24, 22);
    box.removeFromTop (30);

    if (triggerSection != nullptr)
        triggerSection->setBounds (box.removeFromTop (150));
    box.removeFromTop (10);
    helpArea = box.removeFromTop (76);
    box.removeFromTop (12);

    auto row = box.removeFromTop (28);
    sizeLabelArea = row.removeFromLeft (110);
    for (auto& b : scaleButtons)
    {
        b.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (8);
    }

    box.removeFromTop (18);
    auto keysHeader = box.removeFromTop (24);
    resetKeysButton.setBounds (keysHeader.removeFromRight (70));
    keysLabelArea = keysHeader;
    box.removeFromTop (8);
    for (int a = 0; a < KeyMap::numActions; ++a)
    {
        auto keyRow = box.removeFromTop (22);
        keyNameAreas[(size_t) a] = keyRow.removeFromLeft (170);
        for (auto& b : keyButtons[(size_t) a])
        {
            b.setBounds (keyRow.removeFromLeft (140));
            keyRow.removeFromLeft (8);
        }
        box.removeFromTop (4);
    }

    box.removeFromTop (30);
    aboutArea = box.removeFromTop (60);
    box.removeFromTop (6);
    auto links = box.removeFromTop (20);
    siteLink.setBounds (links.removeFromLeft (links.getWidth() / 2));
    licenseLink.setBounds (links);
}

void SettingsOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! boxBounds().contains (e.getPosition()) && onClose)
        onClose();
}

bool SettingsOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key != juce::KeyPress::escapeKey || onClose == nullptr)
        return false;
    onClose();
    return true;
}

//==============================================================================
GearButton::GearButton() : juce::Button ("settings")
{
    setClickingTogglesState (false);
    setTooltip ("Settings: the sidechain trigger, the window size and the display");
}

void GearButton::setSidechainState (bool armed, float activity)
{
    activity = armed ? juce::jlimit (0.0f, 1.0f, activity) : 0.0f;
    if (armed == scArmed && std::abs (activity - scActivity) < 0.01f && (activity > 0.0f) == (scActivity > 0.0f))
        return;
    scArmed = armed;
    scActivity = activity;
    repaint();
}

void GearButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();

    g.setColour (on ? colours::body.withAlpha (down ? 0.7f : 0.85f) : (down ? colours::panelBorder : colours::display));
    g.fillRoundedRectangle (bounds, 5.0f);
    if (! on)
    {
        g.setColour (highlighted ? colours::textDim : colours::panelBorder);
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
    }

    // The gear: eight teeth round a ring.
    const auto c = bounds.getCentre();
    const float rOuter = 8.5f, rInner = 6.4f, rHole = 2.6f;
    juce::Path gear;
    constexpr int teeth = 8;
    for (int i = 0; i < teeth * 4; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) (teeth * 4);
        const float r = (i % 4 == 1 || i % 4 == 2) ? rOuter : rInner;
        const juce::Point<float> pt (c.x + r * std::sin (a), c.y - r * std::cos (a));
        if (i == 0)
            gear.startNewSubPath (pt);
        else
            gear.lineTo (pt);
    }
    gear.closeSubPath();
    gear.addEllipse (c.x - rHole, c.y - rHole, rHole * 2.0f, rHole * 2.0f);
    gear.setUsingNonZeroWinding (false);
    g.setColour (on ? colours::background : (highlighted ? colours::text : colours::textDim.brighter (0.25f)));
    g.fillPath (gear);

    // Sidechain on: a dot in the corner that flashes on each hit.
    if (scArmed)
    {
        const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ bounds.getRight() - 5.0f, bounds.getY() + 5.0f });
        if (scActivity > 0.0f)
        {
            g.setColour (colours::trigger.withAlpha (0.4f * scActivity));
            g.fillEllipse (dot.expanded (3.0f * scActivity));
        }
        g.setColour (colours::trigger.withAlpha (0.55f + 0.45f * scActivity));
        g.fillEllipse (dot);
    }
}

//==============================================================================
HistoryButton::HistoryButton (bool isRedo) : juce::Button (isRedo ? "redo" : "undo"), redo (isRedo)
{
    setClickingTogglesState (false);
}

void HistoryButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool enabled = isEnabled();
    highlighted = highlighted && enabled;

    g.setColour (down && enabled ? colours::panelBorder : colours::display);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (highlighted ? colours::textDim : colours::panelBorder);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    // A curved arrow: an arc over the top that comes down into the head on the left
    // (undo), mirrored for redo.
    const auto c = bounds.getCentre();
    const float r = 6.0f;
    const juce::Point<float> centre (c.x + 0.9f, c.y + 2.0f);
    const float start = juce::degreesToRadians (100.0f), end = juce::degreesToRadians (-78.0f);

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, r, r, 0.0f, start, end, true);

    // The head points along the arc at its end (moving anticlockwise).
    const juce::Point<float> tip (centre.x + r * std::sin (end), centre.y - r * std::cos (end));
    const juce::Point<float> dir (-std::cos (end), -std::sin (end));
    const juce::Point<float> side (-dir.y, dir.x);
    const float headLen = 5.2f, headHalf = 3.8f;
    const auto headTip = tip + dir * (headLen * 0.55f);
    const auto back = tip - dir * (headLen * 0.45f);
    juce::Path head;
    head.addTriangle (headTip, back + side * headHalf, back - side * headHalf);

    juce::Path icon;
    juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (icon, arc);
    icon.addPath (head);
    if (redo)
        icon.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, c.x, c.y));

    const auto colour = ! enabled ? colours::textDim.withAlpha (0.35f)
                      : highlighted ? colours::text
                                    : colours::textDim.brighter (0.25f);
    g.setColour (colour);
    g.fillPath (icon);
}

//==============================================================================
HeaderBar::HeaderBar (DrumFoundryProcessor& p) : processor (p)
{
    saveButton.setTooltip ("Save the current sound as a user preset");
    prevButton.onClick = [this] { processor.getPresets().loadPrevious(); };
    nextButton.onClick = [this] { processor.getPresets().loadNext(); };
    saveButton.onClick = [this] { if (onSaveClicked) onSaveClicked(); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);
    addAndMakeVisible (saveButton);

    undoButton.onClick = [this] { if (onUndo) onUndo(); };
    redoButton.onClick = [this] { if (onRedo) onRedo(); };
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    setHistoryState ({}, {});

    browserButton.setColour (juce::TextButton::buttonOnColourId, colours::body);
    browserButton.onClick = [this] { if (onBrowserClicked) onBrowserClicked(); };
    addAndMakeVisible (browserButton);

    settingsButton.onClick = [this] { if (onSettingsClicked) onSettingsClicked(); };
    addAndMakeVisible (settingsButton);

    presetBox.setColour (juce::ComboBox::textColourId, colours::text);
    presetBox.setJustificationType (juce::Justification::centred);
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0)
            processor.getPresets().load (id - 1);
    };
    addAndMakeVisible (presetBox);

    trigger.onTrigger = [this] { processor.triggerAudition (0.9f); };
    addAndMakeVisible (trigger);

    dragExport.setTooltip ("Drag into your DAW to drop this sound in as a WAV");
    addAndMakeVisible (dragExport);

    addAndMakeVisible (meter);

    keyMap->addChangeListener (this);
    updateShortcutTooltips();
    refreshPresets();
}

HeaderBar::~HeaderBar()
{
    keyMap->removeChangeListener (this);
}

void HeaderBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateShortcutTooltips();
}

void HeaderBar::updateShortcutTooltips()
{
    const auto withKeys = [this] (const juce::String& text, KeyAction action)
    {
        const auto keys = keyMap->describeKeys (action);
        return keys.isEmpty() ? text : text + " (" + keys + ")";
    };
    trigger.setTooltip (withKeys ("Play a hit", KeyAction::play));
    prevButton.setTooltip (withKeys ("Previous preset", KeyAction::previousPreset));
    nextButton.setTooltip (withKeys ("Next preset", KeyAction::nextPreset));
    browserButton.setTooltip (withKeys ("Show or hide the sample browser", KeyAction::toggleBrowser));
    undoButton.setTooltip (undoWhat.isNotEmpty() ? withKeys ("Undo " + undoWhat, KeyAction::undo) : juce::String ("Nothing to undo"));
    redoButton.setTooltip (redoWhat.isNotEmpty() ? withKeys ("Redo " + redoWhat, KeyAction::redo) : juce::String ("Nothing to redo"));
}

void HeaderBar::refreshPresets()
{
    auto& presets = processor.getPresets();
    presets.refresh();

    presetBox.clear (juce::dontSendNotification);
    const auto& entries = presets.getEntries();

    // Grouped: kicks, snares, then the user's kicks and snares (ids stay index + 1).
    int lastGroup = -1;
    for (const int i : presets.getDisplayOrder())
    {
        const auto& e = entries[(size_t) i];
        const bool kick = e.category == PresetManager::Category::kick;
        const int group = (e.isFactory ? 0 : 2) + (kick ? 0 : 1);
        if (group != lastGroup)
        {
            if (lastGroup >= 0)
                presetBox.addSeparator();
            presetBox.addSectionHeading (juce::String (e.isFactory ? "" : "User ") + (kick ? "Kick" : "Snare"));
            lastGroup = group;
        }
        presetBox.addItem (e.name, i + 1);
    }

    updatePresetDisplay();
}

void HeaderBar::setHistoryState (const juce::String& undoText, const juce::String& redoText)
{
    undoWhat = undoText;
    redoWhat = redoText;
    undoButton.setEnabled (undoWhat.isNotEmpty());
    redoButton.setEnabled (redoWhat.isNotEmpty());
    updateShortcutTooltips();
}

void HeaderBar::updatePresetDisplay()
{
    auto& presets = processor.getPresets();
    const int index = presets.getCurrentIndex();
    if (index >= 0)
        presetBox.setSelectedId (index + 1, juce::dontSendNotification);
    else
        presetBox.setText (presets.getCurrentName(), juce::dontSendNotification);
}

void HeaderBar::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRect (getLocalBounds());
    g.setColour (colours::panelBorder);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // Logo: the product name, and "by" and the label's name under it.
    auto logo = logoArea;
    auto word = logo.removeFromTop (logo.getHeight() * 3 / 5);
    // DRUM in Archivo Expanded Bold, FOUNDRY in its ExtraLight. The cut is wide: shrink both
    // by the same factor until DRUM FOUNDRY fits.
    auto wordFont = wordmarkFont (22.0f).withExtraKerningFactor (0.02f);
    auto thinFont = wordmarkThinFont (22.0f).withExtraKerningFactor (0.02f);
    const auto nameWidth = [&wordFont, &thinFont]
    {
        return juce::GlyphArrangement::getStringWidth (wordFont, "DRUM") + 7.0f
             + juce::GlyphArrangement::getStringWidth (thinFont, "FOUNDRY");
    };
    // The version follows the name, small; the name gives it room.
    const juce::String version ("v" + juce::String (DRUMFOUNDRY_VERSION));
    const auto versionFont = uiFont (10.0f, true).withExtraKerningFactor (0.06f);
    const float versionGap = 6.0f;
    const float versionWidth = juce::GlyphArrangement::getStringWidth (versionFont, version);
    const float nameRoom = (float) word.getWidth() - versionGap - versionWidth;
    if (const auto w = nameWidth(); w > nameRoom)
    {
        const float scale = nameRoom / w;
        wordFont = wordFont.withHeight (wordFont.getHeight() * scale);
        thinFont = thinFont.withHeight (thinFont.getHeight() * scale);
    }
    g.setFont (wordFont);
    g.setColour (colours::text);
    g.drawText ("DRUM", word, juce::Justification::bottomLeft);
    const int drumW = (int) juce::GlyphArrangement::getStringWidth (wordFont, "DRUM") + 7;
    g.setFont (thinFont);
    g.setColour (colours::body);
    g.drawText ("FOUNDRY", word.withTrimmedLeft (drumW), juce::Justification::bottomLeft);

    // Debossed into the panel: dark text with a faint highlight on its lower edge (lit from
    // above, as the flow arrows are), on FOUNDRY's baseline.
    {
        const float x = (float) word.getX() + (float) drumW + juce::GlyphArrangement::getStringWidth (thinFont, "FOUNDRY") + versionGap;
        const float baseline = (float) word.getBottom() - thinFont.getDescent();
        const auto drawVersion = [&] (float dy)
        {
            juce::GlyphArrangement glyphs;
            glyphs.addLineOfText (versionFont, version, x, baseline + dy);
            glyphs.draw (g);
        };
        g.setFont (versionFont);
        g.setColour (juce::Colours::white.withAlpha (0.09f));
        drawVersion (1.0f);
        g.setColour (colours::panel.darker (0.75f));
        drawVersion (0.0f);
    }

    const auto byFont = uiFont (11.0f).withExtraKerningFactor (0.04f);
    g.setFont (byFont);
    g.setColour (colours::textDim);
    auto by = logo.withTrimmedTop (2);
    g.drawText ("by", by.translated (1, 0), juce::Justification::centredLeft);
    by.removeFromLeft ((int) juce::GlyphArrangement::getStringWidth (byFont, "by") + 6);
    drawLabelWordmark (g, by.toFloat(), 0.9f);
}

void HeaderBar::resized()
{
    auto area = getLocalBounds().reduced (20, 0);
    logoArea = area.removeFromLeft (264).reduced (0, 9);
    area.removeFromLeft (16);
    browserButton.setBounds (area.removeFromLeft (92).withSizeKeepingCentre (92, 30));
    area.removeFromLeft (8);
    settingsButton.setBounds (area.removeFromLeft (32).withSizeKeepingCentre (32, 30));

    auto right = area.removeFromRight (190);
    meter.setBounds (right.removeFromRight (16).reduced (0, 11));
    right.removeFromRight (14);
    dragExport.setBounds (right.removeFromRight (82).withSizeKeepingCentre (82, 34));
    right.removeFromRight (10);
    trigger.setBounds (right.removeFromRight (42).withSizeKeepingCentre (42, 42));

    auto centre = getLocalBounds().withSizeKeepingCentre (440, 30); // centred on the window
    prevButton.setBounds (centre.removeFromLeft (30));
    centre.removeFromLeft (6);
    saveButton.setBounds (centre.removeFromRight (62));
    centre.removeFromRight (10);

    // Undo and redo just right of SAVE, outside the centred preset group.
    const auto history = saveButton.getBounds().withX (saveButton.getRight() + 10).withWidth (30);
    undoButton.setBounds (history);
    redoButton.setBounds (history.translated (34, 0));
    nextButton.setBounds (centre.removeFromRight (30));
    centre.removeFromRight (6);
    presetBox.setBounds (centre);
}

} // namespace drum::ui
