// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../Parameters.h"

namespace drum::ui
{

using APVTS = juce::AudioProcessorValueTreeState;

/** Rotary knob bound to a parameter. The caption swaps to the live value while
    hovering or dragging. Double-click resets to default.

    A knob whose parameter an LFO can modulate (see modTargetIds()) takes an LFO's
    handle dragged from the LFO tab: dropping it routes that LFO to the parameter.
    While modulated, a ring in the LFO's colour around the knob shows how far the
    LFOs can move it, with a dot where they have it now; right-click to remove one. */
class Knob : public juce::Component,
             public juce::DragAndDropTarget,
             private juce::Timer
{
public:
    Knob (APVTS& state, const juce::String& paramId, const juce::String& caption,
          juce::Colour accent, bool bipolar = false);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    /** Right-click on the dial: the LFOs that modulate it. */
    void mouseDown (const juce::MouseEvent&) override;

    juce::Slider& getSlider() noexcept { return slider; }
    const juce::String& getParamId() const noexcept { return param->paramID; }

    /** Changes the caption (e.g. when a layer's style gives the knob another job). An
        empty tooltip means the parameter's name. */
    void setCaption (const juce::String& newCaption, const juce::String& tooltip = {});
    const juce::String& getCaption() const noexcept { return caption; }

    /** The LFOs (bit n = LFO n + 1) routed to this knob's parameter. */
    int getModulatingLfos() const noexcept;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    void timerCallback() override;
    /** The normalised range the LFOs can reach and where they have it now. False when unmodulated. */
    bool getModulation (float& low, float& high, float& now, int& firstLfo) const noexcept;

    APVTS& state;
    juce::Slider slider;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
    juce::RangedAudioParameter* param = nullptr;
    juce::String caption;
    juce::Colour accent;

    int modTarget = -1; // index into modTargetIds(), or -1
    std::array<std::atomic<float>*, kNumModRoutes> destValues {}, amountValues {};
    int dropLfo = -1;   // an LFO's handle hovering over the knob
    float shownNow = -1.0f, shownLow = -1.0f, shownHigh = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

/** Drag-and-drop description of LFO `index`'s handle ("lfo:1" for LFO 2), and back
    (-1 when it isn't one). */
juce::var lfoDragDescription (int index);
int lfoFromDragDescription (const juce::var& description);

/** The LFO slot (0-based) that routes `lfo` to modulation target `target`, or -1. */
int findLfoSlot (APVTS& state, int lfo, int target);
/** Routes `lfo` to `target` (an index into modTargetIds()), in the slot that already
    has it or the first free one, with `amount` if it's new. One gesture per parameter.
    Returns the slot, or -1 when all of the LFO's slots are in use. */
int routeLfo (APVTS& state, int lfo, int target, float amount = 0.25f);
/** Empties one of an LFO's slots (no target, no amount). */
void clearLfoSlot (APVTS& state, int lfo, int slot);
/** LFO `lfo`'s output now, -1..1 (0 when the state doesn't belong to a Drum Foundry processor). */
float liveLfoValue (APVTS& state, int lfo);

/** Drop-down for a choice parameter, laid out to line up with knobs. With no caption
    the box fills the component (for a title bar). `separateLast` puts a divider
    before the last choice (e.g. "Sample" after the built-in styles). */
class ChoiceSelector : public juce::Component
{
public:
    ChoiceSelector (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accent,
                    bool separateLast = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::ComboBox& getComboBox() noexcept { return box; }

    /** Lists the choices in groups, each under a heading (an empty heading: just a
        separator), in the order given. Choice values (and the ids) don't change, so
        it's display only. Choices left out of every group aren't listed. */
    void setItemGroups (const std::vector<std::pair<juce::String, std::vector<int>>>& groups);

private:
    juce::ComboBox box;
    juce::AudioParameterChoice* choiceParam = nullptr;
    std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    juce::String caption;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceSelector)
};

/** Pill-shaped toggle for a bool parameter. */
class ToggleSwitch : public juce::Component
{
public:
    ToggleSwitch (APVTS& state, const juce::String& paramId, const juce::String& text,
                  juce::Colour accent, const juce::String& tooltip);

    void resized() override { button.setBounds (getLocalBounds()); }

    juce::Button& getButton() noexcept { return button; }

private:
    juce::TextButton button;
    std::unique_ptr<APVTS::ButtonAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleSwitch)
};

/** Small square "M" that mutes a layer, lit while muted. It's bound to the layer's
    on/off parameter the other way round: muted = off. */
class MuteButton : public juce::Button
{
public:
    MuteButton (APVTS& state, const juce::String& onParamId, const juce::String& layerName);

    bool isMuted() const noexcept { return getToggleState(); }
    /** The name the tooltip uses (e.g. when an extra layer's tab is renamed). */
    void setLayerName (const juce::String& name);

    /** Silenced because another layer is soloed (and this one isn't): drawn as an
        outlined M, so it reads as muted without looking switched off. */
    void setImplicitlyMuted (bool shouldBeImplicitlyMuted);
    bool isImplicitlyMuted() const noexcept { return implicitlyMuted; }
    /** Muted, or silenced by another layer's solo. */
    bool isSilent() const noexcept { return isMuted() || implicitlyMuted; }

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    void updateTooltip();

    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::String layerName;
    bool implicitlyMuted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MuteButton)
};

/** Adds `also` to what clicking the button does, after its own action (M, S and Ø keep
    toggling their parameter, and also pick their layer). */
void alsoOnClick (juce::Button& button, std::function<void()> also);

/** Every layer's solo parameter: Body, Noise, Click, then the extra layers. */
const juce::StringArray& soloParameterIds();

/** Small square "S" that solos a layer, lit (yellow) while soloed. While any layer
    is soloed, only soloed layers play.

    Solo is exclusive: soloing a layer takes the solo from every other layer. Cmd- or
    Shift-click adds the layer to the layers already soloed instead. */
class SoloButton : public juce::Button
{
public:
    SoloButton (APVTS& state, const juce::String& soloParamId, const juce::String& layerName);

    bool isSoloed() const noexcept { return getToggleState(); }
    void setLayerName (const juce::String& name);

    /** What a click does: solo this layer alone (or, `additive`, along with the others
        already soloed), or unsolo it if it's soloed. */
    void toggleSolo (bool additive);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    void updateTooltip();

    APVTS& state;
    const juce::String paramId;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::String layerName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoloButton)
};

/** Small square polarity switch (a circle with a slash, Ø), lit while the layer's
    phase is inverted. */
class InvertButton : public juce::Button
{
public:
    InvertButton (APVTS& state, const juce::String& invertParamId, const juce::String& layerName);

    bool isInverted() const noexcept { return getToggleState(); }
    void setLayerName (const juce::String& name);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    void updateTooltip();

    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::String layerName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InvertButton)
};

/** A layer's M, S and (with an invert parameter) Ø side by side (Body's title bar). */
class LayerSwitches : public juce::Component
{
public:
    LayerSwitches (APVTS& state, const juce::String& onParamId, const juce::String& soloParamId,
                   const juce::String& layerName, const juce::String& invertParamId = {});

    static constexpr int idealWidth = 44;         // M and S
    static constexpr int idealWidthWithInvert = 66; // M, S and Ø
    int getIdealWidth() const noexcept { return invert != nullptr ? idealWidthWithInvert : idealWidth; }

    void resized() override;

    MuteButton mute;
    SoloButton solo;
    std::unique_ptr<InvertButton> invert;
};

/** Reads every layer's solo switch (lock-free), to tell whether any is on. */
class SoloWatcher
{
public:
    explicit SoloWatcher (APVTS& state);
    bool anySoloed() const noexcept;

private:
    std::vector<std::atomic<float>*> values;
};

/** Compact rotary for a header row: a caption, then a small dial. The caption swaps to
    the value while hovering or dragging. Bound to a parameter, or free (give it a
    range and a text function). Double-click resets. */
class HeaderDial : public juce::Component
{
public:
    /** Bound to `paramId`. */
    HeaderDial (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accent,
                bool bipolar = false);
    /** Free: 0..1, reset to `defaultValue`; `textFor` formats the value. */
    HeaderDial (const juce::String& caption, juce::Colour accent, double defaultValue,
                std::function<juce::String (double)> textFor);

    juce::Slider& getSlider() noexcept { return slider; }
    int getIdealWidth() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    void setUp (bool bipolar);
    juce::String valueText() const;

    juce::Slider slider;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
    juce::RangedAudioParameter* param = nullptr;
    std::function<juce::String (double)> textFor;
    juce::String caption;
    juce::Colour accent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderDial)
};

/** Small LED for momentary activity (e.g. a sidechain hit). It only draws what it's
    given: the owner feeds it a level that jumps to 1 on an event and decays. */
class ActivityLight : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ActivityLight (juce::Colour accent);

    /** Brightness, 0 (idle) to 1 (just fired). */
    void setActivity (float level);
    /** When not armed the light is drawn as a faint outline (its source is switched off). */
    void setArmed (bool shouldBeArmed);

    void paint (juce::Graphics&) override;

private:
    juce::Colour accent;
    float activity = 0.0f;
    bool armed = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActivityLight)
};

/** Round power switch for an effect, bound to its on/off parameter: the
    accent-lit power symbol while it's on, a dim one while it's off. */
class PowerButton : public juce::Button
{
public:
    PowerButton (APVTS& state, const juce::String& onParamId, juce::Colour accent, const juce::String& effectName);

    bool isOn() const noexcept { return getToggleState(); }
    /** Called whenever the parameter changes (and once, from setOnPowerChanged()). */
    void setOnPowerChanged (std::function<void (bool)> callback);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::Colour accent;
    juce::String name;
    std::function<void (bool)> onPowerChanged;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerButton)
};

/** A small embossed arrow on a plate, set across the gap between two sections to
    show which way the signal flows. It ignores the mouse. */
class FlowArrow : public juce::Component
{
public:
    enum class Direction { right, down };

    explicit FlowArrow (Direction d);

    static constexpr int size = 18;

    void paint (juce::Graphics&) override;

private:
    Direction direction;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlowArrow)
};

/** Titled panel that lays its children out in an even row. */
class Section : public juce::Component
{
public:
    Section (const juce::String& title, juce::Colour accent);

    /** Adds a control that takes one slot (or `slots` slots) of the row. */
    void addControl (juce::Component& c, float slots = 1.0f);
    /** Adds a vertical stack of small components (e.g. toggles) as one slot. */
    void addStack (std::initializer_list<juce::Component*> items, float slots = 1.0f);
    /** Puts a small component (e.g. an ActivityLight, or a button) at the right end of the
        title bar, `width` wide. */
    void setHeaderComponent (juce::Component& c, int width = 20);
    /** Puts a pill (e.g. an on/off switch) above the row of slots. It spans the slots from
        `firstSlot` to the end, with a divider before them if that isn't the whole row. The
        whole row moves down to make room, so every control in the section stays aligned.
        `height` > 0 makes it that tall instead of a pill (e.g. a display over the knobs). */
    void setTopComponent (juce::Component& c, int firstSlot = 0, int height = 0);
    /** Puts a power switch for `onParamId` in place of the title's dot. While it's
        off the section's controls are dimmed (they stay editable). */
    void setPowerParameter (APVTS& state, const juce::String& onParamId);

    float getSlotCount() const noexcept;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    struct Slot
    {
        std::vector<juce::Component*> items;
        float weight = 1.0f;
        bool stack = false;
    };

    juce::String title;
    juce::Colour accent;
    std::vector<Slot> slots;
    juce::Component* headerComponent = nullptr;
    int headerWidth = 20;
    juce::Component* topComponent = nullptr;
    std::unique_ptr<PowerButton> power;
    bool bypassed = false;
    int topFirstSlot = 0;
    int topHeight = 0; // 0: a pill
    juce::Line<float> divider; // empty unless the top pill covers only part of the row

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Section)
};

} // namespace drum::ui
