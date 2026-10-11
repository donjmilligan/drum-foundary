// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"
#include "LfoPanel.h"

namespace drum::ui
{

/** The big display: rendered waveform with the drawable curves on top. Every layer has
    a pitch and a level curve: Body, Noise and Click their own, and each extra layer
    either its own or, coupled, one of the core layers' (its Pitch / Level choice). The
    display shows the selected layer's curve on the tab on show (Pitch or Level): the
    layer is the one picked above (the Body panel, or a Layers tab). An extra layer
    coupled to a core layer's curve shows that curve, and editing it edits the core
    layer's curve too; with Flat there's no curve to draw.
    Double-click adds/removes points, dragging a segment's midpoint bends it,
    scroll/pinch zooms the time axis.

    Time is in units of Length (0..1), which is where the envelopes live. Audio past
    Length (a reverb tail) is drawn dimmer after it, and zooming out reaches it.

    Vertical axes:
    - Pitch: absolute frequency from 0 Hz to 16 kHz on a warped scale, w = log2 (1 + f / 20 Hz):
      close to linear near 0 Hz, and an octave scale above about 50 Hz, so the bottom
      of the axis really is 0 Hz. A Hz ruler and note names. Each curve is stored in
      semitones from its layer's base, and its end point *is* that base (dragging it
      moves the base, and the other points move back so they keep their absolute pitch):
        Body pitch:  Tune (times KEY's transposition),
        Click pitch: Click Tone,
        Noise pitch: the noise band's centre, the geometric mean of Low Cut and High Cut
                     (the end drag moves both),
        an extra layer's own pitch: Tune x its Tune (Sample, Sine, Body; the drag moves
                     its Tune), its band's centre (Noise), or its Tone x its Tune (Click).
      Focus zooms the axis from the full range to a band around the curve; the ruler pans it.
    - Level: linear gain, with headroom above 0 dB up to +12 dB (the "clip zone": the top
      part of the height, in dB). Anything over 0 dB drives the output clipper.

    The reverb gate's marker (shown while the gate is on and the reverb's Mix is up) can
    be dragged: its line sets Gate Time, the end of its shaded fade sets Gate Fade.
    Double-click resets either.

    Under the plot, one timing lane per layer shows where the layer starts relative
    to the hit (its Offset). Drag a lane to move the layer earlier or later; whatever
    starts before 0 ms is cut off (shown hatched). Double-click resets it to 0. */
class EnvelopeCanvas : public juce::Component
{
public:
    explicit EnvelopeCanvas (DrumFoundryProcessor& processor);
    ~EnvelopeCanvas() override;

    enum Tab { pitchTab = 0, levelTab = 1 };
    /** Layers the display can show: Body, Noise, Click, then extra layer k at firstExtraLayer + k. */
    enum { bodyLayer = 0, noiseLayer = 1, clickLayer = 2, firstExtraLayer = 3, numLayers = firstExtraLayer + kNumExtraLayers };
    static int tabOf (int envIndex) noexcept { return isPitchEnvelope (envIndex) ? pitchTab : levelTab; }
    /** "Body pitch", "Noise level", "Layer 2 pitch" ... (see getCurveName() for the layer's own name). */
    static juce::String curveName (int envIndex);

    /** Shows `layer`'s curve on `tab` (pitchTab or levelTab). */
    void showLayer (int layer, int tab);
    int getLayer() const noexcept { return layer; }
    int getTab() const noexcept { return tab; }
    /** The curve on show, or -1 when the layer has none on this tab (an extra layer on Flat). */
    int getActiveEnvelope() const noexcept { return hasCurve ? (int) active : -1; }
    /** True when an extra layer is on show, playing a core layer's curve (coupled to it). */
    bool isCoupled() const noexcept { return hasCurve && layer >= firstExtraLayer && layerOfEnvelope (active) < 0; }
    /** The curve `layer` follows on `tab` (an EnvId), as its parameters stand, or -1 for none. */
    int resolveEnvelope (int layer, int tab) const;

    /** ALL: every other playing layer's curve on this tab is drawn too, dimmed and named,
        behind the one being edited; clicking one picks its layer (onLayerPicked). */
    void setShowAll (bool shouldShowAll);
    bool getShowAll() const noexcept { return showAll; }
    /** The user clicked another layer's dimmed curve (ALL): it's on show now. */
    std::function<void (int layer)> onLayerPicked;
    /** The layer's name as its tab or panel shows it: "BODY", "NOISE", "NOISE 2", "SINE" ... */
    juce::String getLayerName (int layer) const;
    /** "Noise 2 pitch", or for a coupled curve the core layer's ("Body pitch"). */
    juce::String getCurveName() const;
    /** Re-reads which curve the layer on show follows (its Pitch / Level choice may have
        changed: the menu, undo, automation). The editor's timer calls this. */
    void refreshActiveCurve();

    /** Re-reads all envelopes (after a preset or host state load). */
    /** Re-reads all envelopes (after a preset or host state load). */
    void reloadFromProcessor();
    /** Re-reads the envelopes if something other than this display changed them (a new
        layer's curves, a switch to Own). Keeps the view and any gesture as they are. */
    void syncEnvelopes();

    /** The rendered hit, starting at the hit: exactly what's played (after the output
        stage). It can be any length: whatever runs past `lengthMs` is shown as a tail. */
    void setWaveform (const juce::AudioBuffer<float>& hit, double sampleRate, float lengthMs);
    void setPlayhead (float normalisedPosition);
    /** Where the reverb gate starts cutting, in ms after the hit (negative for no marker). */
    void setGateMarker (float gateMs, float fadeMs);
    /** x of the gate line, or -1 when there's no marker (for tests). */
    float getGateMarkerX() const noexcept;

    /** Zooms to fit Length (and any layer that starts early). If it already fits and
        there's a tail, fits the tail too. */
    void fitView();

    /** Semitones the shown note is from C3, when KEY is on (0 when it's off). The pitch
        ruler and the point readout name the notes and frequencies that note plays. */
    void setNoteOffset (int semitones);

    //==============================================================================
    /** Pitch axis zoom: 0 shows 0 Hz - 16 kHz, 1 a tight band around the visible curves. */
    void setPitchFocus (float focus);
    float getPitchFocus() const noexcept { return pitchFocus; }
    /** Pan of the zoomed pitch axis, in axis units (about an octave each above 50 Hz;
        the ruler drag and alt-scroll set it). */
    void setPitchPan (float units);
    float getPitchPan() const noexcept { return pitchPan; }
    /** The pitch range on show, in Hz. */
    juce::Range<float> getPitchViewHz() const;
    /** Focus or pan changed from the canvas itself (scrolling or dragging the ruler). */
    std::function<void()> onPitchViewChanged;

    /** The pitch axis' mapping, both ways (y in this component's coordinates). */
    float pitchHzToY (float hz) const noexcept;
    float pitchYToHz (float y) const noexcept;
    /** What a pitch curve's 0 st plays, in Hz: Tune (with KEY's note), Click Tone, the noise
        band's centre, or an extra layer's own base. */
    float getBaseHz (int envIndex) const noexcept;
    juce::Rectangle<float> getPlotArea() const { return plotArea(); }

    //==============================================================================
    /** Re-reads the layers' offsets, and which layers are on or soloed (the editor's timer). */
    void updateLanes();
    int getNumLanes() const noexcept { return (int) lanes.size(); }
    /** The lane's row (for tests). */
    juce::Rectangle<float> getLaneBounds (int lane) const;
    juce::String getLaneParamId (int lane) const;
    juce::String getLaneName (int lane) const;

    /** Where point `index` of the active envelope is drawn (for tests). */
    juce::Point<float> getPointPosition (int index) const { return pointPosition (index); }
    /** Where point `index` of any envelope would be drawn (for tests). */
    juce::Point<float> getPointPosition (int envIndex, int index) const { return pointPositionOf (envIndex, index); }
    const CurveEnvelope& getEnvelope (EnvId id) const noexcept { return envs[(size_t) id]; }
    /** x for a time in ms after the hit (for tests). */
    float msToX (float ms) const noexcept;

    /** Called after every edit so the owner can re-render the waveform. */
    std::function<void()> onEdited;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

private:
    struct Hit
    {
        enum Kind { none, point, handle, lane, ruler, gate, gateFade };
        Kind kind = none;
        int index = -1;
        bool operator== (const Hit& o) const noexcept { return kind == o.kind && index == o.index; }
    };

    struct Lane
    {
        juce::String name, paramId;
        juce::Colour colour;
        float offsetMs = 0.0f;
        bool silent = false; // muted, or silenced by another layer's solo
        bool operator== (const Lane& o) const noexcept
        {
            return name == o.name && paramId == o.paramId && juce::exactlyEqual (offsetMs, o.offsetMs) && silent == o.silent;
        }
    };

    juce::Rectangle<float> plotArea() const;
    juce::Rectangle<float> laneStrip() const;
    float laneStripHeight() const noexcept;
    float timeToX (float t) const noexcept;
    float xToTime (float x) const noexcept;
    float valueToY (int env, float v) const noexcept;
    float yToValue (int env, float y) const noexcept;
    juce::Point<float> pointPosition (int index) const { return pointPositionOf ((int) active, index); }
    juce::Point<float> pointPositionOf (int env, int index) const;
    juce::Point<float> handlePosition (int index) const;
    bool isGateShown() const noexcept { return gateMs >= 0.0f; }
    /** Points to `env` (shown, or not when `curve` is false) and resets the hover / drag. */
    void setActive (int env, bool curve);

    // Pitch axis
    juce::Range<float> pitchView() const noexcept; // axis units (w)
    float semisToHz (int env, float semis) const noexcept;
    float hzToSemis (int env, float hz) const noexcept;
    void updateFocusBand();
    void setPanClamped (float units);
    /** The end point's drag on a pitch curve: moves its base (Tune, Click Tone, the noise
        band, or an extra layer's Tune, Tone or band) so the end lands on `targetHz`,
        keeping the other points' pitch. */
    void moveBase (float targetHz);
    /** What sets a pitch curve's base: its parameters, and how they map to Hz. */
    struct BaseParams
    {
        enum Kind { hz, semitones, band };
        Kind kind = hz;
        std::vector<juce::String> ids; // one (hz, semitones) or two (band: low, high)
        float ratio = 1.0f;            // hz: base = value x ratio; semitones: base = ratio x 2^(value / 12)
    };
    BaseParams baseParamsFor (int env) const;
    void endGestures();

    void refreshParams();
    Hit findHit (juce::Point<float> p) const;
    int findLane (juce::Point<float> p) const;
    void setHover (Hit h);
    void commit();
    void zoomAround (float anchorTime, float factor);
    void clampView();
    void setLaneOffset (int lane, float ms);
    void setGateParam (bool fade, float ms);

    void drawGrid (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;
    void drawPitchGrid (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawPitchLabels (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawAmpGrid (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;
    void drawWaveform (juce::Graphics&, juce::Rectangle<float> plot) const;
    /** The waveform, top and bottom, around `cy`, full scale `amp` pixels away. */
    void drawWaveformAt (juce::Graphics&, juce::Rectangle<float> plot, float cy, float amp) const;
    /** Level tab: the waveform as played, full scale on the 0 dB line and its mirror. */
    void drawLevelWaveform (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;
    void drawPastLength (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawGateMarker (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;
    void drawEnvelope (juce::Graphics&, juce::Rectangle<float> plot, int envIndex) const;
    /** ALL: the other layers' curves, each envelope once, that are drawn dimmed. */
    struct Ghost { int layer, env; bool playing; };
    std::vector<Ghost> ghostCurves (const DrumParams&) const;
    void drawGhosts (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;
    /** The layer whose dimmed curve passes within a few pixels of `p`, or -1. */
    int ghostAt (juce::Point<float> p) const;
    float ghostY (int envIndex, float x) const;
    void drawLanes (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawHud (juce::Graphics&, juce::Rectangle<float> plot, const DrumParams&) const;

    juce::String describeValue (int envIndex, int pointIndex, float y) const;
    /** What a pitch curve's end point (0 st) is: "TUNE", "CLICK TONE", "NOISE BAND", "SINE TUNE" ... */
    juce::String baseName (int envIndex) const;
    /** What dragging that end point moves: "Tune", "Click Tone", "the noise band", "Sine's Tune" ... */
    juce::String baseDragName (int envIndex) const;

    DrumFoundryProcessor& processor;
    std::array<CurveEnvelope, numEnvs> envs;
    int seenEnvelopeEdits = 0;
    int layer = bodyLayer, tab = pitchTab;
    EnvId active = envPitch;
    bool hasCurve = true; // false: the layer has no curve on this tab (Flat)

    std::vector<float> waveLo, waveHi; // per sample, the lower and the higher of left and right
    float waveSamplesPerUnit = 1.0f;
    float timeLimit = 1.0f; // end of the audible audio (at least Length): as far as the view can go
    float timeMin = 0.0f;   // before 0 when a layer starts early: as far back as the view can go

    float viewStart = 0.0f, viewEnd = 1.0f;
    float playhead = -1.0f;
    float gateMs = -1.0f, gateFadeMs = 0.0f;
    int noteOffset = 0;

    // Cached from the parameters (refreshParams()).
    float tuneHz = 200.0f, lengthMs = 450.0f, clickToneHz = 4500.0f, noiseLowHz = 450.0f, noiseHighHz = 9000.0f;
    struct ExtraLayer
    {
        bool on = false;
        int source = 0, pitchLink = 4, levelLink = 4;
        float tuneSt = 0.0f, lowHz = 20.0f, highHz = 20000.0f, toneHz = 4500.0f;
        juce::String name;
    };
    std::array<ExtraLayer, kNumExtraLayers> extras;
    bool showAll = false;

    float pitchFocus = 0.0f, pitchPan = 0.0f;
    juce::Range<float> focusBand { 3.0f, 6.0f }; // axis units: the band Focus 1 shows

    std::vector<Lane> lanes;

    Hit hover, drag;
    float dragStartY = 0.0f, dragStartX = 0.0f, dragStartCurve = 0.0f, dragStartValue = 0.0f, dragStartHz = 0.0f;
    float dragStartLow = 0.0f, dragStartHigh = 0.0f; // a band (Noise), for its end-point drag
    BaseParams dragBase;                              // the end-point drag's
    // Parameters in a gesture: a pitch curve's base (end-point drag), a lane's Offset, Gate Time or Fade.
    std::vector<juce::RangedAudioParameter*> gestureParams;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeCanvas)
};

/** Which note the display is drawn for. With KEY on, the whole drum transposes by
    (note - C3), and the display follows the last note played: "KEY C1 · −24 st", in
    amber when it isn't C3. Clicking it offers to switch KEY or go back to C3. */
class KeyBadge : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit KeyBadge (DrumFoundryProcessor& processor);

    /** KEY's state and the last note played (the editor polls both). */
    void setState (bool keyOn, int midiNote);
    juce::String getText() const;
    /** True when the display is transposed (KEY on, and not C3). */
    bool isTransposed() const noexcept { return keyOn && note != 60; }
    int getIdealWidth() const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void updateTooltip();

    DrumFoundryProcessor& processor;
    bool keyOn = true;
    int note = 60;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyBadge)
};

/** "CLIP" and a tiny picture of the output clipper's transfer curve (mode, drive and
    ceiling), or "COMP" and the output compressor's curve (in dB). Clicking it opens the
    mode menu (onClick). */
class ClipCurveIcon : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ClipCurveIcon (APVTS& state);

    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    std::atomic<float>* mode = nullptr;
    std::atomic<float>* drive = nullptr;
    std::atomic<float>* ceiling = nullptr;
    std::atomic<float>* threshold = nullptr; // the output compressor's, for Comp
    std::atomic<float>* ratio = nullptr;
    std::atomic<float>* makeup = nullptr;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> listeners; // repaint on change

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipCurveIcon)
};

/** Tabs + tab-specific controls + KEY badge + Length bar + zoom reset, wrapped around
    the canvas. The tabs are PITCH, LEVEL and LFO. Next to the first two, the name of the
    layer on show (the one selected above) and, for an extra layer, where that tab's curve
    comes from: its own (OWN), coupled to BODY, NOISE or CLICK, or FLAT. LFO swaps the
    canvas for the LfoPage (the three LFOs, whose handles drag onto knobs). The Pitch tab
    shows the FOCUS dial; the Level tab shows the output clipper (mode, drive, ceiling),
    since that's where level over 0 dB goes. The layer and tab on show live in the
    processor's UiState, so they survive closing the editor and reloading the session. */
class DisplayPanel : public juce::Component
{
public:
    DisplayPanel (DrumFoundryProcessor& processor);

    EnvelopeCanvas& getCanvas() noexcept { return canvas; }
    KeyBadge& getKeyBadge() noexcept { return keyBadge; }
    HeaderDial& getFocusDial() noexcept { return focusDial; }
    /** The clip mode selector shown on the Level tab. */
    juce::ComboBox& getClipBox() noexcept { return clipBox.getComboBox(); }
    /** EnvelopeCanvas::pitchTab, levelTab, or lfoTab. */
    juce::Button& getTabButton (int tab) noexcept { return tabs[(size_t) juce::jlimit (0, numTabs - 1, tab)]; }
    static constexpr int lfoTab = 2, numTabs = 3;
    LfoPage& getLfoPage() noexcept { return lfoPage; }
    /** True while the LFO tab is on show (instead of the curves). */
    bool isLfoTabShown() const noexcept { return lfoShown; }
    /** Where an extra layer's curve comes from (shown for extra layers only). */
    juce::ComboBox& getSourceBox() noexcept { return sourceBox; }

    /** Shows a tab: EnvelopeCanvas::pitchTab or levelTab (for the layer on show), or lfoTab. */
    void showTab (int tab);
    /** The curves' tab (pitch or level), even while the LFO tab is on show. */
    int getTab() const noexcept { return canvas.getTab(); }
    /** Shows `layer`'s curves (an EnvelopeCanvas layer: 0 Body, 1 Noise, 2 Click, 3 + k
        extra layer k). With `bringToFront`, the curves come back on show if the LFO tab
        was (a click on a layer's tab or its M / S / Ø). */
    void focusLayer (int layer, bool bringToFront = false);
    int getLayer() const noexcept { return canvas.getLayer(); }
    /** Follows the parameters: the layer's name, its curve source, the curve on show (the editor's timer). */
    void update();

    /** The user picked a layer on the display itself (a dimmed curve, with ALL on), so
        the layer panels can follow. */
    std::function<void (int layer)> onLayerFocused;
    /** ALL: shows every layer's curves on the tab together (for tests). */
    juce::Button& getAllButton() noexcept { return allButton; }

    /** KEY's state and the last note played: the badge, the ruler and the readout follow. */
    void setKeyState (bool keyOn, int midiNote);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void updateTabControls();
    /** After the canvas' layer, tab or curve changed: the tabs, the source menu, and UiState. */
    void syncFromCanvas();
    void setLfoShown (bool shouldShow);
    void showSourceMenu();

    DrumFoundryProcessor& processor;
    EnvelopeCanvas canvas;
    KeyBadge keyBadge;
    std::array<juce::TextButton, 3> tabs;
    LfoPage lfoPage;
    bool lfoShown = false;
    juce::ComboBox sourceBox;
    juce::Rectangle<int> layerNameArea;
    juce::String shownLayerName;
    juce::Slider lengthBar;
    std::unique_ptr<APVTS::SliderAttachment> lengthAttachment;
    juce::TextButton fitButton { "FIT" };
    juce::TextButton allButton { "ALL" };

    HeaderDial focusDial;
    ClipCurveIcon clipIcon;
    ChoiceSelector clipBox;
    HeaderDial clipDrive, clipCeiling;
    HeaderDial compThresh, compRatio, compAttack, compRelease, compMakeup; // the output compressor (Clip Mode: Comp)
    ChoiceSelector compStyle;                                               // its character
    std::unique_ptr<juce::ParameterAttachment> clipModeListener;
    std::array<int, 2> dividerX { -1, -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisplayPanel)
};

} // namespace drum::ui
