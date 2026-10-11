// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "Controls.h"
#include "FoundryLookAndFeel.h"
#include "../Parameters.h"
#include "../PluginProcessor.h"

namespace drum::ui
{

namespace
{
    constexpr int kCaptionHeight = 16;
    constexpr int kSectionTitleHeight = 24;
    constexpr int kPillHeight = 22;
}

//==============================================================================
juce::var lfoDragDescription (int index)
{
    return "lfo:" + juce::String (index);
}

int lfoFromDragDescription (const juce::var& description)
{
    const auto text = description.toString();
    if (! text.startsWith ("lfo:"))
        return -1;
    const int index = text.fromFirstOccurrenceOf (":", false, false).getIntValue();
    return juce::isPositiveAndBelow (index, kNumLfos) ? index : -1;
}

namespace
{
    void setPlainAsGesture (APVTS& state, const juce::String& id, float plain)
    {
        if (auto* p = state.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
            p->endChangeGesture();
        }
    }

    float rawValue (APVTS& state, const juce::String& id)
    {
        const auto* v = state.getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    }
}

int findLfoSlot (APVTS& state, int lfo, int target)
{
    for (int k = 0; k < kLfoSlots; ++k)
        if (juce::roundToInt (rawValue (state, lfoParamId (lfo, pid::lfoDest, k))) == target + 1)
            return k;
    return -1;
}

int routeLfo (APVTS& state, int lfo, int target, float amount)
{
    if (! juce::isPositiveAndBelow (lfo, kNumLfos) || ! juce::isPositiveAndBelow (target, modTargetIds().size()))
        return -1;

    int slot = findLfoSlot (state, lfo, target);
    if (slot < 0)
        slot = findLfoSlot (state, lfo, -1); // the first free one
    if (slot < 0)
        return -1;

    if (juce::roundToInt (rawValue (state, lfoParamId (lfo, pid::lfoDest, slot))) != target + 1)
        setPlainAsGesture (state, lfoParamId (lfo, pid::lfoDest, slot), (float) (target + 1));
    if (juce::exactlyEqual (rawValue (state, lfoParamId (lfo, pid::lfoAmt, slot)), 0.0f))
        setPlainAsGesture (state, lfoParamId (lfo, pid::lfoAmt, slot), amount);
    return slot;
}

void clearLfoSlot (APVTS& state, int lfo, int slot)
{
    if (! juce::isPositiveAndBelow (lfo, kNumLfos) || ! juce::isPositiveAndBelow (slot, kLfoSlots))
        return;
    setPlainAsGesture (state, lfoParamId (lfo, pid::lfoDest, slot), 0.0f);
    setPlainAsGesture (state, lfoParamId (lfo, pid::lfoAmt, slot), 0.0f);
}

float liveLfoValue (APVTS& state, int lfo)
{
    if (auto* p = dynamic_cast<DrumFoundryProcessor*> (&state.processor))
        return p->getLfoValue (lfo);
    return 0.0f;
}

//==============================================================================
Knob::Knob (APVTS& s, const juce::String& paramId, const juce::String& text, juce::Colour accentColour, bool bipolar)
    : state (s), param (s.getParameter (paramId)), caption (text), accent (accentColour)
{
    jassert (param != nullptr);

    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setMouseDragSensitivity (220);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.getProperties().set ("bipolar", bipolar);
    slider.setTooltip (param->getName (64));
    slider.addMouseListener (this, false);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    attachment = std::make_unique<APVTS::SliderAttachment> (s, paramId, slider);
    slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    // Modulatable: watch the LFO slots for routes to this parameter.
    modTarget = modTargetIndex (paramId);
    if (modTarget >= 0)
    {
        for (int l = 0; l < kNumLfos; ++l)
            for (int k = 0; k < kLfoSlots; ++k)
            {
                const auto i = (size_t) (l * kLfoSlots + k);
                destValues[i] = s.getRawParameterValue (lfoParamId (l, pid::lfoDest, k));
                amountValues[i] = s.getRawParameterValue (lfoParamId (l, pid::lfoAmt, k));
            }
        startTimerHz (24);
    }
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromBottom (kCaptionHeight);
    const int size = juce::jmin (area.getWidth(), area.getHeight(), 60);
    slider.setBounds (area.withSizeKeepingCentre (size, size));
}

void Knob::setCaption (const juce::String& newCaption, const juce::String& tooltip)
{
    slider.setTooltip (tooltip.isNotEmpty() ? tooltip : param->getName (64));
    if (caption != newCaption)
    {
        caption = newCaption;
        repaint();
    }
}

void Knob::paint (juce::Graphics& g)
{
    const bool showValue = slider.isMouseOverOrDragging();
    const auto area = getLocalBounds().removeFromBottom (kCaptionHeight);

    g.setFont (uiFont (11.5f, showValue));
    g.setColour (showValue ? accent : colours::textDim);
    g.drawFittedText (showValue ? param->getCurrentValueAsText() : caption.toUpperCase(),
                      area, juce::Justification::centred, 1, 0.8f);
}

int Knob::getModulatingLfos() const noexcept
{
    int lfos = 0;
    for (size_t i = 0; i < destValues.size(); ++i)
        if (destValues[i] != nullptr && juce::roundToInt (destValues[i]->load()) == modTarget + 1
            && ! juce::exactlyEqual (amountValues[i]->load(), 0.0f))
            lfos |= 1 << (int) (i / (size_t) kLfoSlots);
    return lfos;
}

bool Knob::getModulation (float& low, float& high, float& now, int& firstLfo) const noexcept
{
    if (modTarget < 0)
        return false;

    const float base = param->getValue();
    float depth = 0.0f, offset = 0.0f;
    firstLfo = -1;
    for (size_t i = 0; i < destValues.size(); ++i)
    {
        if (juce::roundToInt (destValues[i]->load()) != modTarget + 1)
            continue;
        const float amount = amountValues[i]->load();
        if (juce::exactlyEqual (amount, 0.0f))
            continue;

        const int lfo = (int) (i / (size_t) kLfoSlots);
        if (firstLfo < 0)
            firstLfo = lfo;
        depth += std::abs (amount);
        offset += amount * liveLfoValue (state, lfo);
    }
    if (firstLfo < 0)
        return false;

    low = juce::jlimit (0.0f, 1.0f, base - depth);
    high = juce::jlimit (0.0f, 1.0f, base + depth);
    now = juce::jlimit (0.0f, 1.0f, base + offset);
    return true;
}

void Knob::timerCallback()
{
    float low = -1.0f, high = -1.0f, now = -1.0f;
    int lfo = -1;
    if (! getModulation (low, high, now, lfo))
        low = high = now = -1.0f;

    if (! juce::approximatelyEqual (low, shownLow) || ! juce::approximatelyEqual (high, shownHigh)
        || std::abs (now - shownNow) > 0.002f)
    {
        shownLow = low;
        shownHigh = high;
        shownNow = now;
        repaint();
    }
}

void Knob::paintOverChildren (juce::Graphics& g)
{
    // The same geometry as the dial's arc (FoundryLookAndFeel::drawRotarySlider), one ring further out.
    const auto dial = slider.getBounds().toFloat().reduced (2.0f);
    const float radius = juce::jmin (dial.getWidth(), dial.getHeight()) * 0.5f;
    const auto centre = dial.getCentre();
    const auto rotary = slider.getRotaryParameters();
    const float ring = radius + 3.0f;
    const auto angleOf = [&] (float v) { return rotary.startAngleRadians + v * (rotary.endAngleRadians - rotary.startAngleRadians); };

    if (dropLfo >= 0)
    {
        const auto c = colours::lfo (dropLfo);
        g.setColour (c.withAlpha (0.18f));
        g.fillEllipse (juce::Rectangle<float> (ring * 2.0f + 4.0f, ring * 2.0f + 4.0f).withCentre (centre));
        g.setColour (c);
        g.drawEllipse (juce::Rectangle<float> (ring * 2.0f + 2.0f, ring * 2.0f + 2.0f).withCentre (centre), 2.0f);
    }

    float low, high, now;
    int lfo;
    if (! getModulation (low, high, now, lfo))
        return;

    const auto c = colours::lfo (lfo);
    juce::Path range;
    range.addCentredArc (centre.x, centre.y, ring, ring, 0.0f, angleOf (low), angleOf (juce::jmax (high, low + 0.002f)), true);
    g.setColour (c.withAlpha (0.85f));
    g.strokePath (range, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float a = angleOf (now);
    const juce::Point<float> dot (centre.x + std::sin (a) * ring, centre.y - std::cos (a) * ring);
    g.setColour (colours::panel);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (dot));
    g.setColour (c);
    g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (dot));
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || modTarget < 0)
        return;

    const int lfos = getModulatingLfos();
    if (lfos == 0)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Modulated by");
    for (int l = 0; l < kNumLfos; ++l)
    {
        if ((lfos & (1 << l)) == 0)
            continue;
        const int slot = findLfoSlot (state, l, modTarget);
        const auto amount = state.getParameter (lfoParamId (l, pid::lfoAmt, slot));
        menu.addItem ("Remove LFO " + juce::String (l + 1) + " (" + (amount != nullptr ? amount->getCurrentValueAsText() : juce::String()) + ")",
                      [this, l, slot] { clearLfoSlot (state, l, slot); });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (slider));
}

bool Knob::isInterestedInDragSource (const SourceDetails& details)
{
    return modTarget >= 0 && lfoFromDragDescription (details.description) >= 0;
}

void Knob::itemDragEnter (const SourceDetails& details)
{
    dropLfo = lfoFromDragDescription (details.description);
    repaint();
}

void Knob::itemDragExit (const SourceDetails&)
{
    dropLfo = -1;
    repaint();
}

void Knob::itemDropped (const SourceDetails& details)
{
    dropLfo = -1;
    const int lfo = lfoFromDragDescription (details.description);
    if (routeLfo (state, lfo, modTarget) < 0)
    {
        // Every slot of that LFO is taken: say so where the user is looking.
        auto* bubble = new juce::BubbleMessageComponent(); // deletes itself
        if (auto* top = getTopLevelComponent())
            top->addChildComponent (bubble);
        juce::AttributedString text;
        text.append ("LFO " + juce::String (lfo + 1) + " already modulates " + juce::String (kLfoSlots)
                     + " things: remove one on the LFO tab first", uiFont (12.0f), colours::text);
        bubble->showAt (&slider, text, 2600, true, true);
    }
    timerCallback();
    repaint();
}

//==============================================================================
ChoiceSelector::ChoiceSelector (APVTS& state, const juce::String& paramId, const juce::String& text, juce::Colour accent,
                                bool separateLast)
    : caption (text)
{
    auto* param = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId));
    jassert (param != nullptr);
    choiceParam = param;

    // Ids are index + 1, which is what the attachment expects (separators don't count).
    const int count = param->choices.size();
    for (int i = 0; i < count; ++i)
    {
        if (separateLast && i == count - 1 && i > 0)
            box.addSeparator();
        box.addItem (param->choices[i], i + 1);
    }
    box.setColour (juce::ComboBox::textColourId, accent);
    box.setTooltip (param->getName (64));
    addAndMakeVisible (box);

    attachment = std::make_unique<APVTS::ComboBoxAttachment> (state, paramId, box);
}

void ChoiceSelector::setItemGroups (const std::vector<std::pair<juce::String, std::vector<int>>>& groups)
{
    if (choiceParam == nullptr)
        return;
    box.clear (juce::dontSendNotification);
    bool first = true;
    for (const auto& [heading, indices] : groups)
    {
        if (! first)
            box.addSeparator();
        first = false;
        if (heading.isNotEmpty())
            box.addSectionHeading (heading);
        for (int i : indices)
            if (juce::isPositiveAndBelow (i, choiceParam->choices.size()))
                box.addItem (choiceParam->choices[i], i + 1);
    }
    box.setSelectedId (choiceParam->getIndex() + 1, juce::dontSendNotification);
}

void ChoiceSelector::resized()
{
    if (caption.isEmpty())
    {
        box.setBounds (getLocalBounds());
        return;
    }

    auto area = getLocalBounds();
    area.removeFromBottom (kCaptionHeight);
    box.setBounds (area.withSizeKeepingCentre (juce::jmin (area.getWidth() - 4, 86), 26));
}

void ChoiceSelector::paint (juce::Graphics& g)
{
    if (caption.isEmpty())
        return;

    g.setFont (uiFont (11.5f));
    g.setColour (colours::textDim);
    g.drawFittedText (caption.toUpperCase(), getLocalBounds().removeFromBottom (kCaptionHeight),
                      juce::Justification::centred, 1);
}

//==============================================================================
ToggleSwitch::ToggleSwitch (APVTS& state, const juce::String& paramId, const juce::String& text,
                            juce::Colour accent, const juce::String& tooltip)
{
    button.setButtonText (text);
    button.setClickingTogglesState (true);
    button.setColour (juce::TextButton::buttonOnColourId, accent);
    button.setTooltip (tooltip);
    addAndMakeVisible (button);

    attachment = std::make_unique<APVTS::ButtonAttachment> (state, paramId, button);
}

//==============================================================================
void alsoOnClick (juce::Button& button, std::function<void()> also)
{
    button.onClick = [own = std::move (button.onClick), extra = std::move (also)]
    {
        if (own)
            own();
        if (extra)
            extra();
    };
}

//==============================================================================
MuteButton::MuteButton (APVTS& state, const juce::String& onParamId, const juce::String& name)
    : juce::Button ("M"), layerName (name)
{
    auto* param = state.getParameter (onParamId);
    jassert (param != nullptr);

    // Called on the message thread, straight away when the change comes from here.
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float on)
    {
        setToggleState (on < 0.5f, juce::dontSendNotification);
        updateTooltip();
    });
    onClick = [this] { attachment->setValueAsCompleteGesture (isMuted() ? 1.0f : 0.0f); };
    attachment->sendInitialUpdate();
}

void MuteButton::setLayerName (const juce::String& name)
{
    layerName = name;
    updateTooltip();
}

void MuteButton::setImplicitlyMuted (bool shouldBeImplicitlyMuted)
{
    if (implicitlyMuted != shouldBeImplicitlyMuted)
    {
        implicitlyMuted = shouldBeImplicitlyMuted;
        updateTooltip();
        repaint();
    }
}

void MuteButton::updateTooltip()
{
    auto tip = (isMuted() ? "Unmute " : "Mute ") + layerName;
    if (implicitlyMuted && ! isMuted())
        tip << "\nSilent while another layer is soloed";
    setTooltip (tip);
}

void MuteButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool muted = isMuted();

    if (muted)
    {
        g.setColour (colours::mute.withAlpha (down ? 0.7f : (highlighted ? 1.0f : 0.9f)));
        g.fillRoundedRectangle (bounds, 4.0f);
    }
    else if (implicitlyMuted)
    {
        // Silenced by a solo elsewhere: outlined in the mute colour.
        g.setColour (colours::mute.withAlpha (down ? 0.25f : 0.12f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (colours::mute.withAlpha (highlighted ? 0.95f : 0.7f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.2f);
    }
    else
    {
        g.setColour (down ? colours::panelBorder : colours::display);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (highlighted ? colours::textDim : colours::panelBorder);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    g.setFont (uiFont (11.0f, true));
    g.setColour (muted ? colours::background
                       : implicitlyMuted ? colours::mute.withAlpha (0.9f)
                                         : (highlighted ? colours::text : colours::textDim));
    g.drawText ("M", bounds.translated (0.0f, 0.5f), juce::Justification::centred);
}

//==============================================================================
const juce::StringArray& soloParameterIds()
{
    static const juce::StringArray ids = []
    {
        juce::StringArray a { pid::bodySolo, pid::noiseSolo, pid::clickSolo };
        for (int n = 0; n < kNumExtraLayers; ++n)
            a.add (layerParamId (n, pid::laySolo));
        return a;
    }();
    return ids;
}

SoloButton::SoloButton (APVTS& s, const juce::String& soloParamId, const juce::String& name)
    : juce::Button ("S"), state (s), paramId (soloParamId), layerName (name)
{
    auto* param = state.getParameter (soloParamId);
    jassert (param != nullptr);

    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float soloed)
    {
        setToggleState (soloed >= 0.5f, juce::dontSendNotification);
        updateTooltip();
    });
    onClick = [this]
    {
        const auto mods = juce::ModifierKeys::currentModifiers;
        toggleSolo (mods.isCommandDown() || mods.isShiftDown());
    };
    attachment->sendInitialUpdate();
}

void SoloButton::toggleSolo (bool additive)
{
    const bool soloing = ! isSoloed();

    // Exclusive: soloing this layer takes the solo from the others.
    if (soloing && ! additive)
    {
        for (const auto& id : soloParameterIds())
        {
            if (id == paramId)
                continue;
            if (auto* other = state.getParameter (id); other != nullptr && other->getValue() >= 0.5f)
            {
                other->beginChangeGesture();
                other->setValueNotifyingHost (0.0f);
                other->endChangeGesture();
            }
        }
    }

    attachment->setValueAsCompleteGesture (soloing ? 1.0f : 0.0f);
}

void SoloButton::setLayerName (const juce::String& name)
{
    layerName = name;
    updateTooltip();
}

void SoloButton::updateTooltip()
{
    setTooltip (isSoloed() ? "Unsolo " + layerName
                           : "Solo " + layerName + " (takes the solo from the other layers; Cmd- or Shift-click adds it to them)");
}

void SoloButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool soloed = isSoloed();

    if (soloed)
    {
        g.setColour (colours::solo.withAlpha (down ? 0.7f : (highlighted ? 1.0f : 0.92f)));
        g.fillRoundedRectangle (bounds, 4.0f);
    }
    else
    {
        g.setColour (down ? colours::panelBorder : colours::display);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (highlighted ? colours::textDim : colours::panelBorder);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    g.setFont (uiFont (11.0f, true));
    g.setColour (soloed ? colours::background : (highlighted ? colours::text : colours::textDim));
    g.drawText ("S", bounds.translated (0.0f, 0.5f), juce::Justification::centred);
}

//==============================================================================
InvertButton::InvertButton (APVTS& state, const juce::String& invertParamId, const juce::String& name)
    : juce::Button ("invert"), layerName (name)
{
    auto* param = state.getParameter (invertParamId);
    jassert (param != nullptr);

    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float inverted)
    {
        setToggleState (inverted >= 0.5f, juce::dontSendNotification);
        updateTooltip();
    });
    onClick = [this] { attachment->setValueAsCompleteGesture (isInverted() ? 0.0f : 1.0f); };
    attachment->sendInitialUpdate();
}

void InvertButton::setLayerName (const juce::String& name)
{
    layerName = name;
    updateTooltip();
}

void InvertButton::updateTooltip()
{
    setTooltip (juce::String ("Invert polarity (phase)") + (layerName.isNotEmpty() ? " of " + layerName : juce::String())
                + (isInverted() ? ": inverted" : ""));
}

void InvertButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool inverted = isInverted();

    if (inverted)
    {
        g.setColour (colours::invert.withAlpha (down ? 0.7f : (highlighted ? 1.0f : 0.92f)));
        g.fillRoundedRectangle (bounds, 4.0f);
    }
    else
    {
        g.setColour (down ? colours::panelBorder : colours::display);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (highlighted ? colours::textDim : colours::panelBorder);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    // Ø: a ring with a slash through it.
    const auto c = bounds.getCentre().translated (0.0f, 0.25f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.25f;
    g.setColour (inverted ? colours::background : (highlighted ? colours::text : colours::textDim));
    g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c), 1.4f);
    const float s = r * 1.45f;
    g.drawLine (c.x - s * 0.72f, c.y + s * 0.72f, c.x + s * 0.72f, c.y - s * 0.72f, 1.4f);
}

//==============================================================================
LayerSwitches::LayerSwitches (APVTS& state, const juce::String& onParamId, const juce::String& soloParamId,
                              const juce::String& layerName, const juce::String& invertParamId)
    : mute (state, onParamId, layerName), solo (state, soloParamId, layerName)
{
    addAndMakeVisible (mute);
    addAndMakeVisible (solo);
    if (invertParamId.isNotEmpty())
    {
        invert = std::make_unique<InvertButton> (state, invertParamId, layerName);
        addAndMakeVisible (*invert);
    }
}

void LayerSwitches::resized()
{
    auto area = getLocalBounds();
    const int n = invert != nullptr ? 3 : 2;
    const int w = (area.getWidth() - 4 * (n - 1)) / n;
    mute.setBounds (area.removeFromLeft (w));
    area.removeFromLeft (4);
    solo.setBounds (area.removeFromLeft (w));
    if (invert != nullptr)
        invert->setBounds (area.removeFromRight (w));
}

//==============================================================================
SoloWatcher::SoloWatcher (APVTS& state)
{
    for (const auto& id : soloParameterIds())
        values.push_back (state.getRawParameterValue (id));
}

bool SoloWatcher::anySoloed() const noexcept
{
    for (const auto* v : values)
        if (v != nullptr && v->load() >= 0.5f)
            return true;
    return false;
}

//==============================================================================
HeaderDial::HeaderDial (APVTS& state, const juce::String& paramId, const juce::String& text, juce::Colour a, bool bipolar)
    : param (state.getParameter (paramId)), caption (text), accent (a)
{
    jassert (param != nullptr);
    setUp (bipolar);
    slider.setTooltip (param->getName (64));
    attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
}

HeaderDial::HeaderDial (const juce::String& text, juce::Colour a, double defaultValue, std::function<juce::String (double)> fn)
    : textFor (std::move (fn)), caption (text), accent (a)
{
    setUp (false);
    slider.setRange (0.0, 1.0);
    slider.setValue (defaultValue, juce::dontSendNotification);
    slider.setDoubleClickReturnValue (true, defaultValue);
}

void HeaderDial::setUp (bool bipolar)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setMouseDragSensitivity (180);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.getProperties().set ("bipolar", bipolar);
    slider.addMouseListener (this, false);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
}

juce::String HeaderDial::valueText() const
{
    if (param != nullptr)
        return param->getCurrentValueAsText();
    return textFor ? textFor (slider.getValue()) : juce::String (slider.getValue(), 2);
}

int HeaderDial::getIdealWidth() const
{
    // Room for the caption or the widest value, whichever is wider, plus the dial.
    const auto font = uiFont (11.0f, true);
    float w = juce::GlyphArrangement::getStringWidth (font, caption.toUpperCase());
    w = juce::jmax (w, juce::GlyphArrangement::getStringWidth (font, param != nullptr ? juce::String ("-12.0 dB") : juce::String ("100%")));
    return (int) std::ceil (w) + 6 + 26;
}

void HeaderDial::resized()
{
    const int size = juce::jmin (26, getHeight());
    slider.setBounds (getLocalBounds().removeFromRight (size).withSizeKeepingCentre (size, size));
}

void HeaderDial::paint (juce::Graphics& g)
{
    const bool showValue = slider.isMouseOverOrDragging();
    const auto area = getLocalBounds().withTrimmedRight (juce::jmin (26, getHeight()) + 5);
    g.setFont (uiFont (11.0f, true));
    g.setColour (showValue ? accent : colours::textDim);
    g.drawFittedText (showValue ? valueText() : caption.toUpperCase(), area, juce::Justification::centredRight, 1, 0.8f);
}

//==============================================================================
ActivityLight::ActivityLight (juce::Colour a) : accent (a)
{
    setInterceptsMouseClicks (false, false);
}

void ActivityLight::setActivity (float level)
{
    level = juce::jlimit (0.0f, 1.0f, level);
    if (std::abs (level - activity) < 0.01f && (level > 0.0f) == (activity > 0.0f))
        return;

    activity = level;
    repaint();
}

void ActivityLight::setArmed (bool shouldBeArmed)
{
    if (armed != shouldBeArmed)
    {
        armed = shouldBeArmed;
        repaint();
    }
}

void ActivityLight::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const float d = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    const auto led = juce::Rectangle<float> (d, d).withCentre (area.getCentre());
    const float level = armed ? activity : 0.0f;

    // Glow, then the lens: a dark socket that fills with the accent as the level rises.
    if (level > 0.0f)
    {
        g.setColour (accent.withAlpha (0.35f * level));
        g.fillEllipse (led.expanded (d * 0.45f * level));
    }

    g.setColour (colours::display);
    g.fillEllipse (led);
    g.setColour (accent.withAlpha (armed ? 0.25f + 0.75f * level : 0.12f));
    g.fillEllipse (led.reduced (1.0f));
    g.setColour (armed ? accent.withAlpha (0.6f) : colours::panelBorder);
    g.drawEllipse (led, 1.0f);
}

//==============================================================================
PowerButton::PowerButton (APVTS& state, const juce::String& onParamId, juce::Colour a, const juce::String& effectName)
    : juce::Button (effectName + " On"), accent (a), name (effectName)
{
    auto* param = state.getParameter (onParamId);
    jassert (param != nullptr);

    // Called on the message thread, straight away when the change comes from here.
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float on)
    {
        setToggleState (on >= 0.5f, juce::dontSendNotification);
        setTooltip ((isOn() ? "Turn " + name + " off" : "Turn " + name + " on") + " (bypass)");
        if (onPowerChanged)
            onPowerChanged (isOn());
        repaint();
    });
    onClick = [this] { attachment->setValueAsCompleteGesture (isOn() ? 0.0f : 1.0f); };
    attachment->sendInitialUpdate();
}

void PowerButton::setOnPowerChanged (std::function<void (bool)> callback)
{
    onPowerChanged = std::move (callback);
    if (onPowerChanged)
        onPowerChanged (isOn());
}

void PowerButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = isOn();
    const auto c = bounds.getCentre();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());

    g.setColour (on ? accent.withAlpha (down ? 0.3f : (highlighted ? 0.24f : 0.16f))
                    : (down ? colours::panelBorder : colours::display));
    g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (c));
    g.setColour (on ? accent.withAlpha (highlighted ? 0.9f : 0.6f)
                    : (highlighted ? colours::textDim : colours::panelBorder));
    g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (c).reduced (0.5f), 1.0f);

    // The power symbol: an arc open at the top, and a stem through the gap.
    const float r = d * 0.24f;
    const float gap = juce::MathConstants<float>::pi * 0.24f;
    juce::Path symbol;
    symbol.addCentredArc (c.x, c.y + 0.5f, r, r, 0.0f, gap, juce::MathConstants<float>::twoPi - gap, true);
    symbol.startNewSubPath (c.x, c.y + 0.5f - r * 1.3f);
    symbol.lineTo (c.x, c.y + 0.5f - r * 0.25f);
    g.setColour (on ? accent : (highlighted ? colours::text : colours::textDim));
    g.strokePath (symbol, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
FlowArrow::FlowArrow (Direction d) : direction (d)
{
    setInterceptsMouseClicks (false, false);
}

void FlowArrow::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    // The plate bridges the two sections, so it's the panels' colour.
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    // A right-pointing arrow (shaft and head), turned for Down.
    const auto c = bounds.getCentre();
    juce::Path arrow;
    arrow.startNewSubPath (c.x - 5.0f, c.y - 1.5f);
    arrow.lineTo (c.x, c.y - 1.5f);
    arrow.lineTo (c.x, c.y - 4.5f);
    arrow.lineTo (c.x + 5.0f, c.y);
    arrow.lineTo (c.x, c.y + 4.5f);
    arrow.lineTo (c.x, c.y + 1.5f);
    arrow.lineTo (c.x - 5.0f, c.y + 1.5f);
    arrow.closeSubPath();
    if (direction == Direction::down)
        arrow.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi, c.x, c.y));

    // Embossed: lit from above, so a highlight above and a shadow below.
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillPath (arrow, juce::AffineTransform::translation (-0.5f, -1.0f));
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.fillPath (arrow, juce::AffineTransform::translation (0.5f, 1.0f));
    g.setColour (colours::waveform);
    g.fillPath (arrow);
}

//==============================================================================
Section::Section (const juce::String& t, juce::Colour a) : title (t), accent (a)
{
}

void Section::addControl (juce::Component& c, float weight)
{
    addAndMakeVisible (c);
    slots.push_back ({ { &c }, weight });
}

void Section::addStack (std::initializer_list<juce::Component*> items, float weight)
{
    Slot s;
    s.weight = weight;
    s.stack = true;
    for (auto* c : items)
    {
        addAndMakeVisible (c);
        s.items.push_back (c);
    }
    slots.push_back (std::move (s));
}

void Section::setHeaderComponent (juce::Component& c, int width)
{
    headerComponent = &c;
    headerWidth = juce::jmax (1, width);
    addAndMakeVisible (c);
    resized();
}

void Section::setTopComponent (juce::Component& c, int firstSlot, int height)
{
    topComponent = &c;
    topFirstSlot = juce::jmax (0, firstSlot);
    topHeight = juce::jmax (0, height);
    addAndMakeVisible (c);
    resized();
}

void Section::setPowerParameter (APVTS& state, const juce::String& onParamId)
{
    power = std::make_unique<PowerButton> (state, onParamId, accent, title);
    addAndMakeVisible (*power);
    power->setOnPowerChanged ([this] (bool on)
    {
        bypassed = ! on;
        repaint();
    });
    resized();
}

float Section::getSlotCount() const noexcept
{
    float total = 0.0f;
    for (const auto& s : slots)
        total += s.weight;
    return total;
}

void Section::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto titleArea = getLocalBounds().removeFromTop (kSectionTitleHeight).reduced (12, 0);
    if (power != nullptr)
    {
        titleArea.removeFromLeft (power->getRight() + 6 - titleArea.getX());
    }
    else
    {
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) titleArea.getX() + 3.0f,
                                                                         (float) titleArea.getCentreY() + 1.0f }));
        titleArea.removeFromLeft (12);
    }
    g.setFont (titleFont (12.0f));
    g.setColour (bypassed ? colours::textDim : colours::text);
    g.drawText (title.toUpperCase(), titleArea.translated (0, 1), juce::Justification::centredLeft);

    if (divider.getLength() > 0.0f)
    {
        g.setColour (colours::panelBorder);
        g.drawLine (divider, 1.0f);
    }
}

void Section::paintOverChildren (juce::Graphics& g)
{
    // Bypassed: veil the controls (they stay editable), not the title bar.
    if (bypassed)
    {
        g.reduceClipRegion (getLocalBounds().withTrimmedTop (kSectionTitleHeight));
        g.setColour (colours::panel.withAlpha (0.55f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 5.0f);
    }
}

void Section::resized()
{
    auto area = getLocalBounds().reduced (8, 0);
    const auto titleBar = area.removeFromTop (kSectionTitleHeight);

    if (headerComponent != nullptr)
    {
        // A small light keeps the old 20 px slot; anything wider sits a little inside the bar.
        auto slot = titleBar.withLeft (titleBar.getRight() - headerWidth).translated (-4, 1);
        if (headerWidth > 20)
            slot = slot.withSizeKeepingCentre (headerWidth, juce::jmin (slot.getHeight(), 20));
        headerComponent->setBounds (slot);
    }
    if (power != nullptr)
        power->setBounds (juce::Rectangle<int> (16, 16).withCentre ({ titleBar.getX() + 9, titleBar.getCentreY() + 1 }));

    area.removeFromBottom (8);
    divider = {};

    const float total = getSlotCount();
    const float unit = total > 0.0f ? (float) area.getWidth() / total : 0.0f;

    if (topComponent != nullptr)
    {
        auto pillRow = area.removeFromTop (topHeight > 0 ? juce::jmin (topHeight, area.getHeight() / 2) : kPillHeight);
        area.removeFromTop (4);

        // Over part of the row: start the pill at its first slot and divide it off.
        float before = 0.0f;
        for (int i = 0; i < juce::jmin (topFirstSlot, (int) slots.size()); ++i)
            before += slots[(size_t) i].weight;

        if (before > 0.0f)
        {
            const float x = (float) pillRow.getX() + unit * before;
            pillRow.setLeft (juce::roundToInt (x));
            divider = { x, (float) pillRow.getY() + 2.0f, x, (float) area.getBottom() - 2.0f };
        }
        topComponent->setBounds (pillRow.reduced (6, 0));
    }

    if (total <= 0.0f)
        return;

    float x = (float) area.getX();
    for (const auto& s : slots)
    {
        const float w = unit * s.weight;
        const auto cell = juce::Rectangle<float> (x, (float) area.getY(), w, (float) area.getHeight()).toNearestInt();
        x += w;

        if (! s.stack)
        {
            s.items.front()->setBounds (cell);
            continue;
        }

        // Stack: evenly spaced pills, vertically centred.
        const int pillH = kPillHeight, gap = 7;
        const int n = (int) s.items.size();
        const int stackH = n * pillH + (n - 1) * gap;
        int y = cell.getCentreY() - stackH / 2 - kCaptionHeight / 4;
        for (auto* c : s.items)
        {
            c->setBounds (cell.getX() + 6, y, cell.getWidth() - 12, pillH);
            y += pillH + gap;
        }
    }
}

} // namespace drum::ui
