// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace drum::ui
{

namespace colours
{
    inline const juce::Colour background  { 0xff0e1115 };
    inline const juce::Colour panel       { 0xff161a20 };
    inline const juce::Colour panelBorder { 0xff252b34 };
    inline const juce::Colour display     { 0xff0a0c0f };
    inline const juce::Colour grid        { 0xff1b2028 };
    inline const juce::Colour gridStrong  { 0xff262d37 };
    inline const juce::Colour track       { 0xff262c35 };
    inline const juce::Colour text        { 0xffd3dae2 };
    inline const juce::Colour textDim     { 0xff6c7784 };
    inline const juce::Colour waveform    { 0xff3a4552 };

    inline const juce::Colour pitch  { 0xffffa53a };
    inline const juce::Colour body   { 0xff3bd2ea };
    inline const juce::Colour noise  { 0xffff5c9d };
    inline const juce::Colour click  { 0xfff1dc58 };
    inline const juce::Colour drive  { 0xffff6d4f };
    inline const juce::Colour eq     { 0xff72e39c };
    inline const juce::Colour master { 0xffc3cbd4 };
    inline const juce::Colour trigger { 0xffa98bff };
    inline const juce::Colour reverb  { 0xff5b8cff };
    inline const juce::Colour layer   { 0xffb8e04f }; // the extra layers
    inline const juce::Colour output  { 0xffffa53a }; // output filter: the same as the analyzer's handles
    inline const juce::Colour mute    { 0xffe8584f }; // a lit mute button
    inline const juce::Colour solo    { 0xffffc93c }; // a lit solo button
    inline const juce::Colour invert  { 0xffa6c8ff }; // a lit polarity (phase invert) button
    inline const juce::Colour clipZone { 0xffff6d4f }; // amplitude above 0 dB (into the clipper)
    inline const juce::Colour brand    { 0xff2a6aad }; // the label's mark: its navy (#00335D), lifted to read on the dark UI

    juce::Colour forEnvelope (int envIndex);
    /** A chart layer's colour (0 Body, 1 Noise, 2 Click, 3 on the extra layers), as on its
        tab. The display draws the selected layer's curve in it, even when the layer is grouped
        with another layer's curve. */
    juce::Colour forLayer (int chartLayer);

    /** LFO 1, 2 and 3: their handles, waves, and the rings on the knobs they modulate. */
    juce::Colour lfo (int index);
}

/** Text: Inter, regular or semibold. */
juce::Font uiFont (float height, bool bold = false);
/** Section, tab and window titles: Archivo SemiExpanded SemiBold. */
juce::Font titleFont (float height);
/** The product name in the header: Archivo Expanded Bold ("DRUM"). */
juce::Font wordmarkFont (float height);
/** Its thin half: Archivo Expanded ExtraLight ("FOUNDRY"). */
juce::Font wordmarkThinFont (float height);

/** The label's logo (THRSTRS, white on the dark UI), fitted to `area`'s height at its left. */
void drawLabelWordmark (juce::Graphics& g, juce::Rectangle<float> area, float opacity = 1.0f);


class FoundryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FoundryLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    /** Text in the default font (labels, text boxes) comes out in Inter too. */
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
};

} // namespace drum::ui
