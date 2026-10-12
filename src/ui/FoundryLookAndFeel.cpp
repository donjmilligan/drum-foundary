// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "FoundryLookAndFeel.h"
#include <DrumFoundryAssets.h>

namespace drum::ui
{

juce::Colour colours::forEnvelope (int envIndex)
{
    // Every curve, pitch or level, takes its layer's colour (checked by `check`: chart colours).
    switch (envIndex)
    {
        case 0:                 // envPitch
        case 1:  return body;   // envBody
        case 4:                 // envClickPitch
        case 5:  return click;  // envClick
        case 2:                 // envNoise
        case 3:  return noise;  // envNoisePitch
        default: return layer;  // the extra layers' own curves
    }
}

juce::Colour colours::lfo (int index)
{
    static const juce::Colour lfos[] { juce::Colour (0xffc08cff), juce::Colour (0xff4fe3c1), juce::Colour (0xffff8fb8) };
    return lfos[juce::jlimit (0, 2, index)];
}

namespace
{
    // The UI's fonts, compiled in (assets/fonts, SIL Open Font License), so it looks the same
    // on every system: Inter for text, Archivo for titles.
    juce::Typeface::Ptr loadFont (const char* data, int size)
    {
        return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
    }

    const juce::Typeface::Ptr& interRegular()
    {
        static const auto face = loadFont (DrumFoundryAssets::InterRegular_ttf, DrumFoundryAssets::InterRegular_ttfSize);
        return face;
    }

    const juce::Typeface::Ptr& interSemiBold()
    {
        static const auto face = loadFont (DrumFoundryAssets::InterSemiBold_ttf, DrumFoundryAssets::InterSemiBold_ttfSize);
        return face;
    }

    const juce::Typeface::Ptr& archivoSemiExpanded()
    {
        static const auto face = loadFont (DrumFoundryAssets::ArchivoSemiExpandedSemiBold_ttf,
                                           DrumFoundryAssets::ArchivoSemiExpandedSemiBold_ttfSize);
        return face;
    }

    const juce::Typeface::Ptr& archivoExpanded()
    {
        static const auto face = loadFont (DrumFoundryAssets::ArchivoExpandedBold_ttf, DrumFoundryAssets::ArchivoExpandedBold_ttfSize);
        return face;
    }

    const juce::Typeface::Ptr& archivoExpandedExtraLight()
    {
        static const auto face = loadFont (DrumFoundryAssets::ArchivoExpandedExtraLight_ttf,
                                           DrumFoundryAssets::ArchivoExpandedExtraLight_ttfSize);
        return face;
    }

    juce::Font fontWith (const juce::Typeface::Ptr& face, float height)
    {
        return juce::Font (juce::FontOptions (face).withHeight (height));
    }
}

juce::Font uiFont (float height, bool bold)
{
    return fontWith (bold ? interSemiBold() : interRegular(), height);
}

juce::Font titleFont (float height)
{
    return fontWith (archivoSemiExpanded(), height);
}

juce::Font wordmarkFont (float height)
{
    return fontWith (archivoExpanded(), height);
}

juce::Font wordmarkThinFont (float height)
{
    return fontWith (archivoExpandedExtraLight(), height);
}

namespace
{
    /** The label's logo: THRSTRS, white on transparent. */
    const juce::Image& labelWordmark()
    {
        static const juce::Image image = juce::ImageCache::getFromMemory (DrumFoundryAssets::Theres_Tears_Logo_Title_white_site_png,
                                                                          DrumFoundryAssets::Theres_Tears_Logo_Title_white_site_pngSize);
        return image;
    }
}

void drawLabelWordmark (juce::Graphics& g, juce::Rectangle<float> area, float opacity)
{
    const auto& image = labelWordmark();
    if (! image.isValid() || area.getHeight() <= 0.0f)
        return;
    g.setOpacity (opacity);
    g.drawImage (image, area, juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid);
    g.setOpacity (1.0f);
}

FoundryLookAndFeel::FoundryLookAndFeel()
{
    using namespace colours;

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::Slider::rotarySliderOutlineColourId, track);
    setColour (juce::Slider::rotarySliderFillColourId, text);
    setColour (juce::Slider::thumbColourId, text);

    setColour (juce::TextButton::buttonColourId, panel);
    setColour (juce::TextButton::buttonOnColourId, text);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, background);

    setColour (juce::ComboBox::backgroundColourId, display);
    setColour (juce::ComboBox::outlineColourId, panelBorder);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textDim);

    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff12161b));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff232a33));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

    setColour (juce::TextEditor::backgroundColourId, display);
    setColour (juce::TextEditor::outlineColourId, panelBorder);
    setColour (juce::TextEditor::focusedOutlineColourId, body);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, body.withAlpha (0.3f));
    setColour (juce::CaretComponent::caretColourId, body);

    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff1a1f26));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, panelBorder);
}

//==============================================================================
void FoundryLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto area = bounds.withSizeKeepingCentre (size, size);
    const auto centre = area.getCentre();
    const float radius = size * 0.5f;
    const float thickness = juce::jmax (2.5f, radius * 0.11f);
    const float arcRadius = radius - thickness * 0.5f;

    const bool enabled = slider.isEnabled();
    const bool hot = slider.isMouseOverOrDragging();
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (enabled ? 1.0f : 0.35f);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (hot ? colours::track.brighter (0.25f) : colours::track);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc (bipolar knobs grow from 12 o'clock)
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const bool bipolar = (bool) slider.getProperties()["bipolar"];
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    if (std::abs (angle - from) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (accent);
        g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Cap
    const float capRadius = radius * 0.64f;
    const auto cap = juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b323c), cap.getX(), cap.getY(),
                                             juce::Colour (0xff181c22), cap.getX(), cap.getBottom(), false));
    g.fillEllipse (cap);
    g.setColour (juce::Colour (0xff343c47));
    g.drawEllipse (cap, 1.0f);

    // Pointer
    const float s = std::sin (angle), c = -std::cos (angle);
    const juce::Point<float> p1 (centre.x + s * capRadius * 0.3f, centre.y + c * capRadius * 0.3f);
    const juce::Point<float> p2 (centre.x + s * capRadius * 0.86f, centre.y + c * capRadius * 0.86f);
    g.setColour (enabled ? colours::text : colours::textDim);
    g.drawLine ({ p1, p2 }, 2.0f);
}

void FoundryLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                         juce::Slider& slider)
{
    if (style != juce::Slider::LinearBar)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (0.5f);
    const auto accent = slider.findColour (juce::Slider::trackColourId);

    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 4.0f);

    {
        // A bipolar bar fills from its centre.
        juce::Graphics::ScopedSaveState save (g);
        const float pos = juce::jlimit (bounds.getX(), bounds.getRight(), sliderPos);
        if ((bool) slider.getProperties()["bipolar"])
        {
            const float centre = bounds.getCentreX();
            g.reduceClipRegion (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (centre, pos), bounds.getY(),
                                                                          juce::jmax (centre, pos), bounds.getBottom()).toNearestInt());
            g.setColour (accent.withAlpha (0.35f));
        }
        else
        {
            g.reduceClipRegion (bounds.withRight (pos).toNearestInt());
            g.setColour (accent.withAlpha (0.16f));
        }
        g.fillRoundedRectangle (bounds, 4.0f);
    }

    g.setColour (slider.isMouseOverOrDragging() ? colours::textDim : colours::panelBorder);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    const auto text = bounds.reduced (9.0f, 0.0f);
    g.setFont (uiFont (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText (slider.getProperties()["caption"].toString(), text, juce::Justification::centredLeft);
    g.setFont (uiFont (12.0f, true));
    g.setColour (colours::text);
    g.drawText (slider.getTextFromValue (slider.getValue()), text, juce::Justification::centredRight);
}

//==============================================================================
void FoundryLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                             bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto accent = button.findColour (juce::TextButton::buttonOnColourId);
    const bool on = button.getToggleState();

    if ((bool) button.getProperties()["tab"])
    {
        if (on)
        {
            g.setColour (accent.withAlpha (0.10f));
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (accent);
            g.fillRoundedRectangle (bounds.removeFromBottom (2.0f), 1.0f);
        }
        else if (highlighted)
        {
            g.setColour (colours::panelBorder.withAlpha (0.5f));
            g.fillRoundedRectangle (bounds, 4.0f);
        }
        return;
    }

    const float corner = juce::jmin (5.0f, bounds.getHeight() * 0.5f);
    if (on)
    {
        g.setColour (accent.withAlpha (down ? 0.7f : (highlighted ? 0.95f : 0.85f)));
        g.fillRoundedRectangle (bounds, corner);
    }
    else
    {
        g.setColour (down ? colours::panelBorder : colours::display);
        g.fillRoundedRectangle (bounds, corner);
        g.setColour (highlighted ? colours::textDim : colours::panelBorder);
        g.drawRoundedRectangle (bounds, corner, 1.0f);
    }
}

void FoundryLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    const bool on = button.getToggleState();
    const bool tab = (bool) button.getProperties()["tab"];
    const auto accent = button.findColour (juce::TextButton::buttonOnColourId);

    juce::Colour colour;
    if (tab)
        colour = on ? accent : (highlighted ? colours::text : colours::textDim);
    else
        colour = on ? colours::background : (highlighted ? juce::Colours::white : colours::text);

    g.setColour (colour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f));
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
}

juce::Font FoundryLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    const bool tab = (bool) button.getProperties()["tab"];
    if (tab)
        return titleFont (juce::jmin (12.0f, (float) buttonHeight * 0.55f));
    return uiFont (juce::jmin (12.0f, (float) buttonHeight * 0.55f), true);
}

//==============================================================================
void FoundryLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int,
                                     juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (box.isMouseOver (true) ? colours::textDim : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    const float arrowSize = 4.0f;
    const float ax = (float) width - 12.0f;
    const float ay = (float) height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (ax - arrowSize, ay - arrowSize * 0.5f);
    arrow.lineTo (ax, ay + arrowSize * 0.5f);
    arrow.lineTo (ax + arrowSize, ay - arrowSize * 0.5f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font FoundryLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return uiFont (juce::jmin (13.0f, (float) box.getHeight() * 0.55f), true);
}

void FoundryLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (juce::Justification::centredLeft);
}

juce::Font FoundryLookAndFeel::getPopupMenuFont()
{
    return uiFont (14.0f);
}

void FoundryLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                                   const juce::String& sectionName)
{
    g.setFont (uiFont (11.0f, true));
    g.setColour (findColour (juce::PopupMenu::headerTextColourId));
    g.drawFittedText (sectionName.toUpperCase(), area.getX() + 12, area.getY(), area.getWidth() - 16,
                      (int) ((float) area.getHeight() * 0.8f), juce::Justification::bottomLeft, 1);
}

//==============================================================================
juce::Typeface::Ptr FoundryLookAndFeel::getTypefaceForFont (const juce::Font& font)
{
    if (font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return font.isBold() ? interSemiBold() : interRegular();
    return LookAndFeel_V4::getTypefaceForFont (font);
}

//==============================================================================
juce::Rectangle<int> FoundryLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                         juce::Rectangle<int> parentArea)
{
    // One line per '\n'.
    const auto font = uiFont (13.0f);
    const auto lines = juce::StringArray::fromLines (tipText);
    float widest = 0.0f;
    for (const auto& line : lines)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (font, line));

    const int w = (int) widest + 18;
    const int h = 8 + 16 * juce::jmax (1, lines.size());
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h).constrainedWithin (parentArea);
}

void FoundryLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (uiFont (13.0f));

    auto area = bounds.toNearestInt().reduced (8, 4);
    for (const auto& line : juce::StringArray::fromLines (text))
        g.drawFittedText (line, area.removeFromTop (16), juce::Justification::centredLeft, 1);
}

} // namespace drum::ui
