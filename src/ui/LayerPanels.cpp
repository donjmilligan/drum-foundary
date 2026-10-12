// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "LayerPanels.h"
#include "FoundryLookAndFeel.h"
#include "../dsp/StyleInfo.h"

namespace drum::ui
{

namespace
{
    constexpr int kTitleHeight = 32;
    constexpr int kTitleGap = 6; // between the title bar (or the tabs) and what's under it
    constexpr int kPadding = 10;
    constexpr int kCaptionHeight = 16; // as Knob
    constexpr int kStripHeight = 30;
    constexpr int kStripControlHeight = 24; // as the sample slot's buttons
    constexpr int kInvertWidth = 22, kInvertHeight = 22; // a page's Ø, left of its selectors
    constexpr float kOffAlpha = 0.45f;    // a layer that's muted
    constexpr float kUnusedAlpha = 0.38f; // a knob the style doesn't use

    // Indices into layerSourceNames and layerEnvNames (StyleInfo.h). An extra Noise
    // layer's style and Click layer's type take the core NoiseStyle / ClickType values.
    constexpr int sourceSample = layerSample, sourceSine = layerSine, sourceNoise = layerNoise,
                  sourceClick = layerClick, sourceBody = layerBody;
    constexpr int kNumSources = numLayerSources;
    constexpr int kLayerNoiseWhite = noiseWhite, kLayerNoiseMetallic = noiseMetallic, kLayerClickSnap = clickSnap;

    const juce::String emDash (juce::CharPointer_UTF8 (" \xe2\x80\x94 "));

    /** The Body Style menus, grouped: the kick bodies, the snare heads, the styles that do
        both (the factory presets use them for kicks and snares), the basic waveforms, and
        (the Body layer's) Sample. */
    std::vector<std::pair<juce::String, std::vector<int>>> bodyStyleGroups (bool withSample)
    {
        std::vector<std::pair<juce::String, std::vector<int>>> g {
            { "Kick",          { bodyKick, bodyRound, bodyTaper, bodyHard, bodyFold, bodyTonal } },
            { "Snare",         { bodyClassic, bodyMembrane, bodyWood } },
            { "Kick or Snare", { body808, body909, bodyFM } },
            { "Basic",         { bodySine, bodySaw, bodySquare, bodyRoundSquare, bodyTriangle } } };
        if (withSample)
            g.push_back ({ {}, { bodySample } });
        return g;
    }

    float rawValue (const std::atomic<float>* v) noexcept
    {
        return v != nullptr ? v->load() : 0.0f;
    }

    void setParamNotifying (juce::RangedAudioParameter* param, float plainValue)
    {
        if (param == nullptr)
            return;
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
        param->endChangeGesture();
    }

    /** What an extra layer's source plays, for the line next to its selectors.
        StyleInfo.h names the knobs; this describes the sound. */
    juce::String layerSourceHint (int source, int style)
    {
        if (source == sourceBody)
            return "Another body in its own style, at Tune plus this Tune, on its own curves";
        if (source == sourceSine)
            return "A sine at Tune plus this layer's Tune, following its pitch curve";
        if (source == sourceClick)
            return "A second transient, with its own tone and decay";
        if (source != sourceNoise)
            return {};

        switch (style)
        {
            case 0:  return "Noise that the body modulates, like snare wires";
            case 1:  return "White noise through the layer's own filters";
            case 2:  return "Pink noise (-3 dB/oct) through the layer's own filters";
            case 3:  return "Six square waves at 808 cymbal ratios (Pitch and Color tune them)";
            case 4:  return "Sparse, dusty crackle";
            default: return {};
        }
    }
}

//==============================================================================
GridPanel::GridPanel (const juce::String& t, juce::Colour a, int c) : title (t), accent (a), columns (juce::jmax (1, c))
{
}

void GridPanel::setTitle (const juce::String& newTitle)
{
    if (title != newTitle)
    {
        title = newTitle;
        resized();
        repaint();
    }
}

void GridPanel::setFrameless (bool shouldBeFrameless)
{
    frameless = shouldBeFrameless;
}

void GridPanel::setTitleLeft (juce::Component* c, int width, int height)
{
    titleLeft = c;
    titleLeftWidth = width;
    titleLeftHeight = height;
}

void GridPanel::addTitleRight (juce::Component& c, int width, int height)
{
    titleRight.push_back ({ &c, width, height });
}

void GridPanel::setStrip (juce::Component* c, int height)
{
    strip = c;
    stripHeight = height;
}

void GridPanel::addStripLeft (juce::Component& c, int width, int height)
{
    stripLeft.push_back ({ &c, width, height });
}

void GridPanel::addCell (juce::Component& c, int pillHeight)
{
    cells.push_back ({ &c, pillHeight });
}

void GridPanel::clearCells()
{
    cells.clear();
}

void GridPanel::setHighlighted (bool shouldBeHighlighted)
{
    if (highlighted != shouldBeHighlighted)
    {
        highlighted = shouldBeHighlighted;
        repaint();
    }
}

void GridPanel::paint (juce::Graphics& g)
{
    if (frameless)
        return;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (highlighted ? accent : colours::panelBorder);
    g.drawRoundedRectangle (bounds.reduced (highlighted ? 0.5f : 0.0f), 6.0f, highlighted ? 1.5f : 1.0f);

    // Without a component before the title, the accent dot marks the panel (as Section).
    if (titleLeft == nullptr)
    {
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) titleText.getX() - 9.0f,
                                                                         (float) titleText.getCentreY() + 0.5f }));
    }

    g.setFont (titleFont (12.5f));
    g.setColour (colours::text);
    g.drawText (title.toUpperCase(), titleText, juce::Justification::centredLeft, true);
}

void GridPanel::resized()
{
    auto area = getLocalBounds();

    if (! frameless)
    {
        area = area.reduced (kPadding, 0);
        auto bar = area.removeFromTop (kTitleHeight);
        const int barCentre = bar.getCentreY() + 1;

        if (titleLeft != nullptr)
        {
            titleLeft->setBounds (bar.removeFromLeft (titleLeftWidth).withHeight (titleLeftHeight)
                                     .withY (barCentre - titleLeftHeight / 2));
            bar.removeFromLeft (10);
        }
        else
        {
            bar.removeFromLeft (14); // the dot
        }

        for (const auto& item : titleRight)
        {
            if (! item.component->isVisible())
                continue;
            item.component->setBounds (bar.removeFromRight (item.width).withHeight (item.height).withY (barCentre - item.height / 2));
            bar.removeFromRight (6);
        }

        const int textWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (titleFont (12.5f), title.toUpperCase())) + 2;
        titleText = bar.removeFromLeft (juce::jmin (bar.getWidth(), textWidth)).withTrimmedTop (1);
        titleFree = bar.withTrimmedLeft (8);

        area.removeFromTop (kTitleGap);
        area.removeFromBottom (6);
    }

    content = area;
    stripArea = {};

    if (stripHeight > 0)
    {
        auto row = area.removeFromTop (stripHeight);
        for (const auto& item : stripLeft)
        {
            if (! item.component->isVisible())
                continue;
            item.component->setBounds (row.removeFromLeft (item.width).withSizeKeepingCentre (item.width, item.height));
            row.removeFromLeft (6);
        }
        stripArea = row;
        if (strip != nullptr)
            strip->setBounds (row);
        area.removeFromTop (6);
    }

    if (cells.empty())
        return;

    const int rows = ((int) cells.size() + columns - 1) / columns;
    const float cellW = (float) area.getWidth() / (float) columns;
    const float cellH = (float) area.getHeight() / (float) rows;

    for (size_t i = 0; i < cells.size(); ++i)
    {
        const int row = (int) i / columns, col = (int) i % columns;
        const auto cell = juce::Rectangle<float> ((float) area.getX() + cellW * (float) col, (float) area.getY() + cellH * (float) row,
                                                  cellW, cellH).toNearestInt();
        const auto& c = cells[i];
        if (c.pillHeight <= 0)
        {
            c.component->setBounds (cell);
            continue;
        }

        // Centred on where a knob's dial would be (above its caption).
        const int centreY = cell.getY() + (cell.getHeight() - kCaptionHeight) / 2;
        const int w = juce::jmin (cell.getWidth() - 16, 86);
        c.component->setBounds (cell.getCentreX() - w / 2, centreY - c.pillHeight / 2, w, c.pillHeight);
    }
}

//==============================================================================
LayerPanel::LayerPanel (DrumFoundryProcessor& p, const juce::String& t, juce::Colour a, SampleSlot slot,
                        const char* onId, const char* soloId, const char* styleId, const char* invertId,
                        UsesSampleFn uses, int columns, bool asPage)
    : GridPanel (t, a, columns),
      onParamId (onId),
      soloParamId (soloId),
      soloWatcher (p.getState()),
      usesSample (uses),
      styleBox (p.getState(), styleId, {}, a, true),
      slotWidget (p, slot, a)
{
    auto& state = p.getState();
    onValue = state.getRawParameterValue (onId);
    soloValue = state.getRawParameterValue (soloId);
    styleValue = state.getRawParameterValue (styleId);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (styleId)))
        styleNames = choice->choices;

    styleBox.getComboBox().setTooltip ("The " + t.toLowerCase() + " sound: a built-in style, or Sample to play an imported file");
    if (juce::String (styleId) == pid::bodyStyle)
        styleBox.setItemGroups (bodyStyleGroups (true));

    addAndMakeVisible (styleBox);
    addAndMakeVisible (slotWidget);
    setStrip (&slotWidget, kStripHeight);

    if (asPage)
    {
        setFrameless (true);
        pageInvert = std::make_unique<InvertButton> (state, invertId, "the " + t.toLowerCase() + " layer");
        addAndMakeVisible (*pageInvert);
        addStripLeft (*pageInvert, kInvertWidth, kInvertHeight);
        addStripLeft (styleBox, 118, kStripControlHeight);
    }
    else
    {
        switches = std::make_unique<LayerSwitches> (state, onId, soloId, "the " + t.toLowerCase() + " layer", invertId);
        addAndMakeVisible (*switches);
        setTitleLeft (switches.get(), switches->getIdealWidth(), 20);
        addTitleRight (styleBox, 118);
    }
}

void LayerPanel::addKnob (Knob& knob, CaptionFn captionFor)
{
    addAndMakeVisible (knob);
    addCell (knob);
    entries.push_back ({ &knob, knob.getCaption(), knob.getSlider().getTooltip(), captionFor, false });
    shownStyle = -1;
}

void LayerPanel::addSampleKnob (Knob& knob)
{
    addKnob (knob);
    entries.back().sampleOnly = true;
}

void LayerPanel::update()
{
    const bool on = rawValue (onValue) >= 0.5f;
    const int style = juce::roundToInt (rawValue (styleValue));
    const bool silent = soloWatcher.anySoloed() && rawValue (soloValue) < 0.5f;
    if ((on ? 1 : 0) != shownOn || style != shownStyle || (silent ? 1 : 0) != shownSilent)
    {
        shownOn = on ? 1 : 0;
        shownStyle = style;
        shownSilent = silent ? 1 : 0;
        if (switches != nullptr)
            switches->mute.setImplicitlyMuted (silent);
        apply (on, style);
        repaint();
    }
}

void LayerPanel::paint (juce::Graphics& g)
{
    GridPanel::paint (g);

    // Say why a layer that's on is silent (the page's tab shows it for Noise and Click).
    if (switches != nullptr && shownSilent == 1 && shownOn == 1)
    {
        g.setFont (uiFont (11.0f, true));
        g.setColour (colours::mute.withAlpha (0.8f));
        g.drawText ("SILENT: ANOTHER LAYER IS SOLOED", getTitleFreeArea(), juce::Justification::centredLeft, true);
    }
}

void LayerPanel::apply (bool on, int style)
{
    const auto styleName = styleNames[style];
    const float layerAlpha = on && shownSilent != 1 ? 1.0f : kOffAlpha;

    for (auto& e : entries)
    {
        bool used = true;
        const char* relabel = nullptr;
        if (e.sampleOnly)
            used = usesSample (style);
        else if (e.captionFor != nullptr)
            used = (relabel = e.captionFor (style)) != nullptr;

        juce::String tooltip;
        if (! used)
            tooltip = e.paramName + emDash + "not used by " + styleName;
        else if (relabel != nullptr && ! e.paramName.endsWithIgnoreCase (relabel))
            tooltip = juce::String (relabel) + " (" + e.paramName + ")";

        e.knob->setCaption (relabel != nullptr ? juce::String (relabel) : e.caption, tooltip);
        e.knob->setAlpha (layerAlpha * (used ? 1.0f : kUnusedAlpha));
    }

    // The slot stays usable (loading a file switches the style to Sample), just quieter.
    slotWidget.setAlpha (on ? (usesSample (style) ? 1.0f : 0.75f) : kOffAlpha);
    styleBox.setAlpha (layerAlpha);
    if (pageInvert != nullptr)
        pageInvert->setAlpha (layerAlpha);
}

//==============================================================================
/** A borderless × (an extra layer's tab uses it to remove the layer). */
class LayersSection::GlyphButton : public juce::Button
{
public:
    GlyphButton() : juce::Button ("remove") {}

    void paintButton (juce::Graphics& g, bool isHighlighted, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat();
        if (isHighlighted)
        {
            g.setColour (colours::panelBorder.withAlpha (down ? 1.0f : 0.8f));
            g.fillRoundedRectangle (bounds, 3.0f);
        }

        const auto cross = bounds.withSizeKeepingCentre (7.0f, 7.0f);
        g.setColour (isHighlighted ? colours::text : colours::textDim);
        g.drawLine ({ cross.getTopLeft(), cross.getBottomRight() }, 1.4f);
        g.drawLine ({ cross.getTopRight(), cross.getBottomLeft() }, 1.4f);
    }
};

//==============================================================================
/** One tab: the layer's name, its mute button and, for an extra layer, ×. Clicking
    anywhere but the buttons shows the layer. The name dims while it's muted. */
class LayersSection::Tab : public juce::Component, public juce::SettableTooltipClient
{
public:
    Tab (APVTS& state, const juce::String& onId, const juce::String& soloId, juce::Colour a, bool removable)
        : mute (state, onId, "this layer"), solo (state, soloId, "this layer"), accent (a)
    {
        addAndMakeVisible (mute);
        addAndMakeVisible (solo);
        if (removable)
        {
            close = std::make_unique<GlyphButton>();
            addAndMakeVisible (*close);
        }
        setRepaintsOnMouseActivity (true);
    }

    std::function<void()> onSelect;

    void setText (const juce::String& newText)
    {
        if (text == newText)
            return;

        text = newText;
        const auto name = text.substring (0, 1) + text.substring (1).toLowerCase(); // "Noise 2"
        mute.setLayerName (name);
        solo.setLayerName (name);
        setTooltip ("Show " + name + " (M mutes it and S solos it, and show it too)");
        if (close != nullptr)
            close->setTooltip ("Remove " + name + " (switches the layer off and closes its tab)");
        repaint();
    }

    const juce::String& getText() const noexcept { return text; }

    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    /** Repaints the name when the layer is muted, unmuted, or silenced by another
        layer's solo (`anySoloed`). */
    void syncMuted (bool anySoloed)
    {
        mute.setImplicitlyMuted (anySoloed && ! solo.isSoloed());
        if (mute.isSilent() != shownMuted)
        {
            shownMuted = mute.isSilent();
            repaint();
        }
    }

    int getIdealWidth() const
    {
        const int textWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (titleFont (12.5f), text));
        return 9 + textWidth + 7 + kSwitch + 2 + kSwitch + (close != nullptr ? 2 + 14 : 0) + 5;
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const bool hover = isMouseOver (true);
        if (selected)
        {
            g.setColour (accent.withAlpha (0.10f));
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (accent);
            g.fillRoundedRectangle (bounds.removeFromBottom (2.0f), 1.0f);
        }
        else if (hover)
        {
            g.setColour (colours::panelBorder.withAlpha (0.5f));
            g.fillRoundedRectangle (bounds, 4.0f);
        }

        auto colour = selected ? accent : (hover ? colours::text : colours::textDim);
        if (mute.isSilent())
            colour = colour.withMultipliedAlpha (0.45f);

        g.setFont (titleFont (12.5f));
        g.setColour (colour);
        g.drawText (text, textArea, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().withTrimmedLeft (9).withTrimmedRight (5);
        if (close != nullptr)
        {
            close->setBounds (area.removeFromRight (14).withSizeKeepingCentre (14, 16));
            area.removeFromRight (2);
        }
        solo.setBounds (area.removeFromRight (kSwitch).withSizeKeepingCentre (kSwitch, 18));
        area.removeFromRight (2);
        mute.setBounds (area.removeFromRight (kSwitch).withSizeKeepingCentre (kSwitch, 18));
        area.removeFromRight (7);
        textArea = area.withTrimmedBottom (1);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onSelect)
            onSelect();
    }

    MuteButton mute;
    SoloButton solo;
    std::unique_ptr<GlyphButton> close;

private:
    static constexpr int kSwitch = 18; // M and S
    juce::Colour accent;
    juce::String text;
    juce::Rectangle<int> textArea;
    bool selected = false, shownMuted = false;
};

//==============================================================================
/** One extra layer's page. The strip row has its polarity (Ø) and source, then its
    style (Body, Noise) or type (Click), then the sample slot (Sample) or a line saying
    what it plays. The grid keeps its places whatever the source:
        Level  Tune   Low Cut  High Cut
        Pan    Offset Env      Color (Noise) / Shape (Body: captioned per style)
                      Length   Tone  (Click: its own decay instead of an envelope)
    Tune is "Pitch" on a Noise layer, and only does something for Metallic. */
struct LayersSection::ExtraPage : public GridPanel
{
    ExtraPage (DrumFoundryProcessor& p, int index)
        : GridPanel ("Layer " + juce::String (index + 1), colours::layer, 4),
          invert (p.getState(), layerParamId (index, pid::layInvert), "this layer"),
          source (p.getState(), layerParamId (index, pid::laySource), {}, colours::layer),
          bodyStyle (p.getState(), layerParamId (index, pid::layBodyStyle), {}, colours::layer),
          noiseStyle (p.getState(), layerParamId (index, pid::layNoiseStyle), {}, colours::layer),
          clickType (p.getState(), layerParamId (index, pid::layClickType), {}, colours::layer),
          slot (p, (SampleSlot) (slotLayer1 + index), colours::layer),
          level (p.getState(), layerParamId (index, pid::layLevel), "Level", colours::layer),
          tune (p.getState(), layerParamId (index, pid::layTune), "Tune", colours::layer, true),
          lowCut (p.getState(), layerParamId (index, pid::layHP), "Low Cut", colours::layer),
          highCut (p.getState(), layerParamId (index, pid::layLP), "High Cut", colours::layer),
          pan (p.getState(), layerParamId (index, pid::layPan), "Pan", colours::layer, true),
          delay (p.getState(), layerParamId (index, pid::layDelay), "Offset", colours::layer, true),
          color (p.getState(), layerParamId (index, pid::layColor), "Color", colours::layer),
          tone (p.getState(), layerParamId (index, pid::layTone), "Tone", colours::layer),
          length (p.getState(), layerParamId (index, pid::layLength), "Length", colours::layer)
    {
        setFrameless (true);

        source.getComboBox().setTooltip ("What this layer plays:\n"
                                         "Sample: its own imported file\n"
                                         "Sine: a sine at Tune (plus this layer's Tune), following its pitch curve\n"
                                         "Noise: a noise layer with its own style and filters\n"
                                         "Click: a second transient\n"
                                         "Body: another drum body, in its own style");
        bodyStyle.getComboBox().setTooltip ("The body style this layer plays");
        bodyStyle.setItemGroups (bodyStyleGroups (false));
        noiseStyle.getComboBox().setTooltip ("The noise this layer plays");
        clickType.getComboBox().setTooltip ("The transient this layer plays");
        delay.getSlider().setTooltip ("Offset: starts the layer later (+) or earlier (-). Earlier cuts off the part\n"
                                      "before the hit. Also draggable on the display's timing lanes.");
        tone.getSlider().setTooltip ("The click's filter / pitch");
        length.getSlider().setTooltip ("How long the click rings");

        for (auto* c : std::initializer_list<juce::Component*> { &invert, &source, &bodyStyle, &noiseStyle, &clickType, &slot,
                                                                 &level, &tune, &lowCut, &highCut, &pan, &delay, &color, &tone, &length })
            addChildComponent (c);
        invert.setVisible (true);
        source.setVisible (true);

        addStripLeft (invert, kInvertWidth, kInvertHeight);
        addStripLeft (source, 96, kStripControlHeight);
        addStripLeft (bodyStyle, 110, kStripControlHeight);
        addStripLeft (noiseStyle, 110, kStripControlHeight);
        addStripLeft (clickType, 110, kStripControlHeight);
    }

    /** `isOn` false dims the page: the layer is muted, or silent under another layer's solo. */
    void apply (bool isOn, int src, int style)
    {
        const bool isSample = src == sourceSample, isNoise = src == sourceNoise, isClick = src == sourceClick,
                   isBody = src == sourceBody;

        bodyStyle.setVisible (isBody);
        noiseStyle.setVisible (isNoise);
        clickType.setVisible (isClick);
        slot.setVisible (isSample);
        setStrip (isSample ? &slot : nullptr, kStripHeight);
        hint = layerSourceHint (src, style);

        clearCells();
        for (auto* c : std::initializer_list<juce::Component*> { &level, &tune, &lowCut, &highCut, &pan, &delay })
        {
            c->setVisible (true);
            addCell (*c);
        }
        length.setVisible (isClick);
        if (isClick)
            addCell (length);
        color.setVisible (isNoise || isBody);
        tone.setVisible (isClick);
        if (isNoise || isBody)
            addCell (color);
        else if (isClick)
            addCell (tone);

        // Noise has no pitch, except Metallic's square waves.
        const bool tuneUsed = ! isNoise || style == kLayerNoiseMetallic;
        juce::String tuneTip;
        if (isNoise)
            tuneTip = tuneUsed ? "Pitch of the Metallic bank, in semitones" : "Pitch" + emDash + "only used by Metallic";
        else if (isSample)
            tuneTip = "Transposes the sample, in semitones";
        else if (src == sourceSine)
            tuneTip = "Semitones from Tune (the body's pitch); -12 is a sub an octave down";
        else if (isBody)
            tuneTip = "Semitones from Tune (the body's pitch)";
        tune.setCaption (isNoise ? "Pitch" : "Tune", tuneTip);

        // Color, Shape and Tone take their names from the engine (Metallic's Color is its
        // pitch; a body's Shape is Strike, Punch, Drive or Index; Stick, Rim and Pop's
        // Tone is a pitch).
        bool colorUsed = true;
        if (isNoise)
        {
            if (const auto* caption = layerColorCaption (src, style))
                color.setCaption (caption, style == kLayerNoiseMetallic ? "Pitch of the Metallic bank" : "Darkens the noise as it decays");
        }
        else if (isBody)
        {
            const auto* caption = layerShapeCaption (layerBody, juce::jlimit (0, (int) bodySample - 1, style));
            colorUsed = caption != nullptr;
            color.setCaption (caption != nullptr ? juce::String (caption) : juce::String ("Shape"),
                              juce::String (caption != nullptr ? caption : "Shape") + " of this body ("
                                  + layerBodyStyleNames[style] + ")");
        }
        if (isClick)
            if (const auto* caption = layerClickToneCaption (src, style))
                tone.setCaption (caption, tone.getSlider().getTooltip());

        const float layerAlpha = isOn ? 1.0f : kOffAlpha;
        for (auto* c : std::initializer_list<juce::Component*> { &invert, &source, &bodyStyle, &noiseStyle, &clickType, &level,
                                                                 &lowCut, &highCut, &pan, &delay, &tone, &length })
            c->setAlpha (layerAlpha);
        tune.setAlpha (layerAlpha * (tuneUsed ? 1.0f : kUnusedAlpha));
        color.setAlpha (layerAlpha * (colorUsed ? 1.0f : kUnusedAlpha));
        slot.setAlpha (layerAlpha);

        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (hint.isEmpty())
            return;

        g.setFont (uiFont (11.5f));
        g.setColour (colours::textDim);
        g.drawFittedText (hint, getStripArea().withTrimmedLeft (4), juce::Justification::centredLeft, 1, 0.9f);
    }

    InvertButton invert;
    ChoiceSelector source, bodyStyle, noiseStyle, clickType;
    SampleSlotWidget slot;
    Knob level, tune, lowCut, highCut, pan, delay, color, tone, length;
    juce::String hint;
};

//==============================================================================
LayersSection::LayersSection (DrumFoundryProcessor& p, LayerPanel& noise, LayerPanel& click)
    : processor (p), noisePage (noise), clickPage (click), soloWatcher (p.getState())
{
    auto& state = p.getState();

    addChildComponent (noisePage);
    addChildComponent (clickPage);
    noisePage.getSlotWidget().onBrowse = [this] (SampleSlot s) { if (onBrowse) onBrowse (s); };
    clickPage.getSlotWidget().onBrowse = [this] (SampleSlot s) { if (onBrowse) onBrowse (s); };

    tabs[noiseTab] = std::make_unique<Tab> (state, noisePage.getOnParamId(), noisePage.getSoloParamId(), colours::noise, false);
    tabs[clickTab] = std::make_unique<Tab> (state, clickPage.getOnParamId(), clickPage.getSoloParamId(), colours::click, false);
    tabs[noiseTab]->setText ("NOISE");
    tabs[clickTab]->setText ("CLICK");

    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& page = extraPages[(size_t) k];
        page = std::make_unique<ExtraPage> (p, k);
        page->slot.onBrowse = [this] (SampleSlot s) { if (onBrowse) onBrowse (s); };
        addChildComponent (*page);

        auto& v = values[(size_t) k];
        v.on = state.getRawParameterValue (layerParamId (k, pid::layOn));
        v.source = state.getRawParameterValue (layerParamId (k, pid::laySource));
        v.noiseStyle = state.getRawParameterValue (layerParamId (k, pid::layNoiseStyle));
        v.bodyStyle = state.getRawParameterValue (layerParamId (k, pid::layBodyStyle));
        v.clickType = state.getRawParameterValue (layerParamId (k, pid::layClickType));
        v.solo = state.getRawParameterValue (layerParamId (k, pid::laySolo));

        auto& tab = tabs[(size_t) (firstExtraTab + k)];
        tab = std::make_unique<Tab> (state, layerParamId (k, pid::layOn), layerParamId (k, pid::laySolo), colours::layer, true);
        tab->close->onClick = [this, k] { removeLayer (k); };
    }

    // Clicking a tab, its M or S, or its page's Ø shows that layer (and its curves).
    for (int t = 0; t < numTabs; ++t)
    {
        auto& tab = *tabs[(size_t) t];
        tab.onSelect = [this, t] { touchTab (t); };
        alsoOnClick (tab.mute, [this, t] { touchTab (t); });
        alsoOnClick (tab.solo, [this, t] { touchTab (t); });
        alsoOnClick (getInvertButton (t), [this, t] { touchTab (t); });
        addChildComponent (tab);
    }

    // A press anywhere on a page counts as touching its layer (see onLayerTouched).
    noisePage.addMouseListener (this, true);
    clickPage.addMouseListener (this, true);
    for (auto& page : extraPages)
        page->addMouseListener (this, true);

    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);

    // Muted layers keep their tabs, and the same tab is shown, while the editor is
    // closed, as long as the patch hasn't been replaced since.
    const auto& ui = processor.uiState;
    if (ui.layerTabsVersion == processor.getStateVersion())
    {
        kept = ui.layerTabs;
        selected = juce::jlimit (0, numTabs - 1, ui.layerTab);
    }

    update();
}

LayersSection::~LayersSection()
{
    noisePage.removeMouseListener (this);
    clickPage.removeMouseListener (this);
    for (auto& page : extraPages)
        page->removeMouseListener (this);
}

void LayersSection::touchTab (int tab)
{
    selectTab (tab);
    if (onLayerTouched)
        onLayerTouched (selected, true);
}

void LayersSection::mouseDown (const juce::MouseEvent& e)
{
    // Only presses on the pages (the tabs report themselves).
    if (e.eventComponent == this || e.mods.isPopupMenu())
        return;
    if (onLayerTouched)
        onLayerTouched (selected, true); // a press anywhere on a page brings its curves into view
}

void LayersSection::update()
{
    bool changed = firstUpdate;
    firstUpdate = false;
    const bool anySoloed = soloWatcher.anySoloed();

    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        const auto i = (size_t) k;
        const auto& v = values[i];
        const bool isOn = rawValue (v.on) >= 0.5f;
        const int source = juce::roundToInt (rawValue (v.source));
        const int style = source == sourceNoise ? juce::roundToInt (rawValue (v.noiseStyle))
                        : source == sourceClick ? juce::roundToInt (rawValue (v.clickType))
                        : source == sourceBody  ? juce::roundToInt (rawValue (v.bodyStyle)) : 0;

        const bool isSilent = anySoloed && rawValue (v.solo) < 0.5f;
        changed = changed || isOn != on[i] || source != sources[i] || style != styles[i] || (isOn && ! kept[i])
               || isSilent != silent[i];
        on[i] = isOn;
        silent[i] = isSilent;
        sources[i] = source;
        styles[i] = style;
        if (isOn)
            kept[i] = true; // from now on it keeps its tab when it's muted
    }

    if (changed)
        refresh();

    noisePage.update();
    clickPage.update();

    // The names dim while muted or silenced by a solo (the buttons follow by themselves).
    for (auto& tab : tabs)
        tab->syncMuted (anySoloed);
}

void LayersSection::resetSelection()
{
    kept.fill (false);
    selected = noiseTab;
    firstUpdate = true;
    update(); // keeps the layers that are on
}

void LayersSection::restoreTabs()
{
    const auto& ui = processor.uiState;
    if (ui.layerTabsVersion == processor.getStateVersion())
        kept = ui.layerTabs;
    else
        kept.fill (false);

    firstUpdate = true;
    update(); // keeps the layers that are on

    // An extra layer the restore switched off, without a tab of its own: show the tab to its left.
    if (const int k = getSelectedExtraLayer(); k >= 0 && ! on[(size_t) k] && ! kept[(size_t) k])
    {
        int tab = selected - 1;
        while (tab >= firstExtraTab && ! isPresent (tab - firstExtraTab))
            --tab;
        selectTab (tab);
    }
}

void LayersSection::selectTab (int tab)
{
    selected = juce::isPositiveAndBelow (tab, (int) numTabs) ? tab : noiseTab;
    refresh();
}

void LayersSection::showSlot (SampleSlot slot)
{
    if (slot == slotNoise)
        selectTab (noiseTab);
    else if (slot == slotClick)
        selectTab (clickTab);
    else if (slot >= slotLayer1 && slot < numSampleSlots)
        selectExtraLayer (slot - slotLayer1);
}

bool LayersSection::isPresent (int index) const noexcept
{
    const auto i = (size_t) index;
    return on[i] || kept[i] || selected == firstExtraTab + index;
}

bool LayersSection::hasTab (int index) const noexcept
{
    return juce::isPositiveAndBelow (index, kNumExtraLayers) && isPresent (index);
}

int LayersSection::findFreeLayer() const noexcept
{
    // The first layer without a tab. Failing that, a layer that only has a tab because
    // it's selected (it's off, and was just being looked at).
    for (int k = 0; k < kNumExtraLayers; ++k)
        if (! on[(size_t) k] && ! kept[(size_t) k] && selected != firstExtraTab + k)
            return k;

    const int k = getSelectedExtraLayer();
    return k >= 0 && ! on[(size_t) k] && ! kept[(size_t) k] ? k : -1;
}

int LayersSection::addLayer (int source)
{
    const int k = findFreeLayer();
    if (k < 0 || ! juce::isPositiveAndBelow (source, layerSourceNames.size()))
        return -1;

    processor.getUndoHistory().noteUserActivity(); // one undo step, even from the "+" menu
    auto& state = processor.getState();
    auto set = [&] (const char* suffix, float plain) { setParamNotifying (state.getParameter (layerParamId (k, suffix)), plain); };

    // Fresh settings (the sample slot keeps its file), then what this kind of layer starts with.
    for (auto* suffix : { pid::layLevel, pid::layTune, pid::layEnv, pid::layPitchEnv, pid::layDelay, pid::layHP, pid::layLP,
                          pid::layPan, pid::layNoiseStyle, pid::layClickType, pid::layColor, pid::layTone, pid::layLength,
                          pid::layInvert, pid::layBodyStyle })
        if (auto* param = state.getParameter (layerParamId (k, suffix)))
            set (suffix, param->convertFrom0to1 (param->getDefaultValue()));

    set (pid::laySource, (float) source);

    // It plays its own curves (Pitch and Level on Own), which start as copies of the
    // core layer's it's most like: the body's for a Body or Sine layer, the Noise
    // layer's for Noise. A Click layer starts flat (its own decay alone), and a Sample
    // layer on the body's level with no pitch movement.
    int pitchFrom = -1, levelFrom = -1;
    if (source == sourceBody)
    {
        // The main body's style (Classic when it plays a sample) and Shape.
        const auto raw = [&state] (const char* id) { const auto* v = state.getRawParameterValue (id); return v != nullptr ? v->load() : 0.0f; };
        const int mainStyle = juce::roundToInt (raw (pid::bodyStyle));
        const bool synthesised = juce::isPositiveAndBelow (mainStyle, layerBodyStyleNames.size());
        set (pid::layBodyStyle, synthesised ? (float) mainStyle : (float) bodyClassic);
        if (synthesised)
            set (pid::layColor, raw (pid::bodyShape));
        pitchFrom = envPitch;
        levelFrom = envBody;
    }
    else if (source == sourceNoise)
    {
        set (pid::layNoiseStyle, (float) kLayerNoiseWhite);
        pitchFrom = envNoisePitch;
        levelFrom = envNoise;
    }
    else if (source == sourceClick)
    {
        set (pid::layClickType, (float) kLayerClickSnap);
    }
    else if (source == sourceSine)
    {
        set (pid::layTune, -12.0f);
        pitchFrom = envPitch;
        levelFrom = envBody;
    }
    else
    {
        levelFrom = envBody;
    }

    const int ownPitch = layerOwnPitchEnvelope (k), ownLevel = layerOwnLevelEnvelope (k);
    processor.setEnvelope ((EnvId) ownPitch, pitchFrom >= 0 ? CurveEnvelope (processor.getEnvelope ((EnvId) pitchFrom).getPoints(), envelopeRange (ownPitch))
                                                            : CurveEnvelope ({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (ownPitch)));
    processor.setEnvelope ((EnvId) ownLevel, levelFrom >= 0 ? CurveEnvelope (processor.getEnvelope ((EnvId) levelFrom).getPoints(), envelopeRange (ownLevel))
                                                            : CurveEnvelope ({ { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } }, envelopeRange (ownLevel)));
    set (pid::layOn, 1.0f);

    kept[(size_t) k] = true;
    firstUpdate = true;
    update();
    selectExtraLayer (k);
    if (onLayerTouched)
        onLayerTouched (selected, true); // the display shows the new layer's curves

    if (source == sourceSample && onBrowse)
        onBrowse ((SampleSlot) (slotLayer1 + k));
    return k;
}

void LayersSection::removeLayer (int index)
{
    if (! juce::isPositiveAndBelow (index, kNumExtraLayers))
        return;

    processor.getUndoHistory().noteUserActivity();
    setParamNotifying (processor.getState().getParameter (layerParamId (index, pid::layOn)), 0.0f);
    // A solo left on a removed layer would silence every other layer.
    setParamNotifying (processor.getState().getParameter (layerParamId (index, pid::laySolo)), 0.0f);
    on[(size_t) index] = false;
    kept[(size_t) index] = false;

    // Show the tab to its left.
    if (selected == firstExtraTab + index)
    {
        int tab = selected - 1;
        while (tab >= firstExtraTab && ! isPresent (tab - firstExtraTab))
            --tab;
        selected = tab;
        if (onLayerTouched)
            onLayerTouched (selected, true);
    }
    refresh();
}

juce::String LayersSection::getTabName (int tab) const
{
    if (! juce::isPositiveAndBelow (tab, (int) numTabs) || ! tabs[(size_t) tab]->isVisible())
        return {};
    return tabs[(size_t) tab]->getText();
}

MuteButton& LayersSection::getMuteButton (int tab)
{
    return tabs[(size_t) juce::jlimit (0, numTabs - 1, tab)]->mute;
}

SoloButton& LayersSection::getSoloButton (int tab)
{
    return tabs[(size_t) juce::jlimit (0, numTabs - 1, tab)]->solo;
}

InvertButton& LayersSection::getInvertButton (int tab)
{
    tab = juce::jlimit (0, numTabs - 1, tab);
    if (tab == noiseTab)
        return noisePage.getInvertButton();
    if (tab == clickTab)
        return clickPage.getInvertButton();
    return extraPages[(size_t) (tab - firstExtraTab)]->invert;
}

void LayersSection::showAddMenu()
{
    const bool free = findFreeLayer() >= 0;

    juce::PopupMenu menu;
    menu.addSectionHeader (free ? "Add a layer" : "All " + juce::String (kNumExtraLayers) + " extra layers are in use");
    menu.addItem ("Add Body layer", free, false, [this] { addLayer (sourceBody); });
    menu.addItem ("Add Noise layer", free, false, [this] { addLayer (sourceNoise); });
    menu.addItem ("Add Click layer", free, false, [this] { addLayer (sourceClick); });
    menu.addItem ("Add Sample layer", free, false, [this] { addLayer (sourceSample); });
    menu.addItem ("Add Sine sub", free, false, [this] { addLayer (sourceSine); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton).withMinimumWidth (170));
}

void LayersSection::refresh()
{
    // Extra tabs are named by what they play, numbered after the core layer of that
    // kind ("NOISE 2") or among themselves when there's more than one ("SAMPLE 1").
    std::array<int, kNumSources> total {}, count {};
    for (int k = 0; k < kNumExtraLayers; ++k)
        if (isPresent (k))
            ++total[(size_t) juce::jlimit (0, kNumSources - 1, sources[(size_t) k])];

    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        const int src = juce::jlimit (0, kNumSources - 1, sources[(size_t) k]);
        const int n = ++count[(size_t) src];
        auto name = layerSourceNames[src].toUpperCase();
        if (src == sourceNoise || src == sourceClick || src == sourceBody)
            name << " " << (n + 1);
        else if (total[(size_t) src] > 1)
            name << " " << n;
        tabs[(size_t) (firstExtraTab + k)]->setText (name);

        auto& page = *extraPages[(size_t) k];
        page.invert.setLayerName (name.substring (0, 1) + name.substring (1).toLowerCase());
        page.apply (on[(size_t) k] && ! silent[(size_t) k], sources[(size_t) k], styles[(size_t) k]);
        page.setVisible (selected == firstExtraTab + k);
    }

    for (int t = 0; t < numTabs; ++t)
    {
        auto& tab = *tabs[(size_t) t];
        tab.setVisible (t < firstExtraTab || isPresent (t - firstExtraTab));
        tab.setSelected (t == selected);
    }

    noisePage.setVisible (selected == noiseTab);
    clickPage.setVisible (selected == clickTab);

    const bool free = findFreeLayer() >= 0;
    addButton.setEnabled (free);
    addButton.setTooltip (free ? "Add a layer: another body, noise, a click, a sample or a sine sub"
                               : "All " + juce::String (kNumExtraLayers) + " extra layers are in use: remove one (x on its tab) to add another");

    storeUiState();
    resized();
    repaint();
}

void LayersSection::storeUiState()
{
    auto& ui = processor.uiState;
    ui.layerTab = selected;
    ui.layerTabs = kept;
    ui.layerTabsVersion = processor.getStateVersion();
}

void LayersSection::setHighlighted (bool shouldBeHighlighted)
{
    if (highlighted != shouldBeHighlighted)
    {
        highlighted = shouldBeHighlighted;
        repaint();
    }
}

void LayersSection::paint (juce::Graphics& g)
{
    // Outlined in the colour of the layer on show (the browser shows the tab it loads into).
    const auto accent = selected == noiseTab ? colours::noise : (selected == clickTab ? colours::click : colours::layer);
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (highlighted ? accent : colours::panelBorder);
    g.drawRoundedRectangle (bounds.reduced (highlighted ? 0.5f : 0.0f), 6.0f, highlighted ? 1.5f : 1.0f);

    // A hairline under the tabs, so the page reads as belonging to the selected one.
    const float y = (float) kTitleHeight + 1.5f;
    g.setColour (colours::panelBorder.withAlpha (0.7f));
    g.drawHorizontalLine ((int) y, (float) kPadding, (float) getWidth() - (float) kPadding);
}

void LayersSection::resized()
{
    auto area = getLocalBounds().reduced (kPadding, 0);
    auto bar = area.removeFromTop (kTitleHeight);
    const int tabHeight = 26;
    const int tabY = bar.getBottom() - tabHeight + 2; // the selected tab's underline sits on the hairline

    for (auto& tab : tabs)
    {
        if (! tab->isVisible())
            continue;
        const int w = tab->getIdealWidth();
        tab->setBounds (bar.removeFromLeft (w).withY (tabY).withHeight (tabHeight));
        bar.removeFromLeft (3);
    }
    bar.removeFromLeft (2);
    addButton.setBounds (bar.removeFromLeft (24).withY (tabY + 2).withHeight (22));

    area.removeFromTop (kTitleGap);
    area.removeFromBottom (6);
    for (auto* page : std::initializer_list<juce::Component*> { &noisePage, &clickPage })
        page->setBounds (area);
    for (auto& page : extraPages)
        page->setBounds (area);
}

} // namespace drum::ui
