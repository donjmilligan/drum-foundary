// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include "../dsp/ParametricEq.h"
#include "Controls.h"

namespace drum::ui
{

/** A resonance in a spectrum: a narrow peak standing out of its surroundings. */
struct EqPeak
{
    float hz = 0.0f;
    float prominenceDb = 0.0f; // how far it stands above the spectrum around it (1/3 octave each way)
    float q = 6.0f;            // from its width at 3 dB down
};

/** Finds up to `maxPeaks` resonances in a spectrum given on a log frequency grid (`hz`
    ascending, `levelDb` the level at each), from 30 Hz to 16 kHz: local maxima at least
    3 dB above the spectrum's 1/3-octave average around them and no more than 40 dB under
    its loudest point, the most prominent first, at least a quarter octave apart. */
std::vector<EqPeak> findResonances (const std::vector<float>& hz, const std::vector<float>& levelDb, int maxPeaks = 5);

/** The colour of EQ band `band` (0-based): its node, curve and tab. */
juce::Colour eqBandColour (int band);

//==============================================================================
/** The EQ's graph: 20 Hz - 20 kHz across, gain up and down (Range: +-12, 24 or 36 dB).
    Behind it, the spectrum of the rendered hit (after the EQ: what you hear). On it,
    each band that's on has its curve and a numbered node, and the EQ's whole curve is
    drawn over them; a dynamic band also shows the gain it's applying now.

    - Drag a node: frequency across, gain up and down (a cut, notch or band pass only
      moves across). Shift: fine. Scroll over a node: its Q (a cut's slope).
    - Double-click empty space: a new band there (the first band that's off): a low cut
      under 30 Hz, a high cut over 16 kHz, else a bell at that gain. Double-click a
      node: that band off.
    - Alt (Option) held on a node: Listen to it (a band pass at its frequency) while the
      mouse is down. Alt-drag on empty space: sweep Listen across the spectrum, to hunt
      for a resonance by ear.
    - Right-click a node: its type, slope, Dynamic, or switch it off.
    - FIND PEAKS (the overlay's) marks the hit's resonances; click a mark to notch it
      with a free band. */
class EqGraph : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit EqGraph (DrumFoundryProcessor& processor);
    ~EqGraph() override;

    /** The newest render of the hit (after the EQ). */
    void setHitAudio (const juce::AudioBuffer<float>& hit, double sampleRate);

    int getSelectedBand() const noexcept { return selected; }
    void selectBand (int band);
    std::function<void (int)> onBandSelected;

    /** The small version in the EQ panel: the curve, the nodes (drag, scroll for Q) and the
        spectrum, with fewer labels and none of the menus, Listen or peak marks. Double-click
        opens the full EQ (onOpenFull). */
    void setCompact (bool shouldBeCompact);
    bool isCompact() const noexcept { return compact; }
    std::function<void()> onOpenFull;

    /** Gain shown above and below 0 dB: 12, 24 or 36. */
    void setRangeDb (float db);
    float getRangeDb() const noexcept { return rangeDb; }

    /** Marks the hit's resonances (findResonances()); returns how many. */
    int findPeaks();
    void clearPeaks() { peaks.clear(); repaint(); }
    const std::vector<EqPeak>& getPeaks() const noexcept { return peaks; }
    /** Notches peak `index` with the first band that's off: a bell cut by the peak's
        prominence (up to 12 dB) at its Q. Returns the band, or -1 when none is free. */
    int fixPeak (int index);

    /** A new band at a point on the graph (the double-click). Returns it, or -1 when all are on. */
    int addBandAt (juce::Point<float> position);

    // Geometry (component coordinates), for drawing and tests.
    juce::Rectangle<float> plotArea() const;
    float freqToX (float hz) const noexcept;
    float xToFreq (float x) const noexcept;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;
    /** Where band `band`'s node is (whether it's on or not). */
    juce::Point<float> getNodePosition (int band) const;
    /** The EQ's whole curve at `hz`, in dB (bands that are on, at their own gain). */
    float getCurveDb (float hz) const;

    void paint (juce::Graphics&) override;
    void resized() override { repaint(); }
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    int findNode (juce::Point<float> p) const;
    int findPeakMark (juce::Point<float> p) const;
    juce::Rectangle<float> peakMarkBounds (int index) const;
    void showBandMenu (int band);
    void setBandParam (int band, const char* suffix, float plainValue);
    float bandParam (int band, const char* suffix) const;
    void startListen (float hz, float q);
    void stopListen();
    double displayRate() const noexcept;

    DrumFoundryProcessor& processor;
    DrumParams params; // as of the last paint / timer tick
    int selected = 1, hovered = -1, hoveredPeak = -1, dragging = -1;
    bool sweeping = false, listening = false, compact = false;
    float rangeDb = 24.0f;
    float dragStartFreqX = 0.0f, dragStartDb = 0.0f;
    juce::Point<float> dragStart;
    std::vector<juce::RangedAudioParameter*> gestureParams;

    // The hit's spectrum on a log grid (1/48 octave), and its loudest point.
    std::vector<float> spectrumHz, spectrumDb;
    float spectrumMax = -200.0f;
    std::vector<EqPeak> peaks;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqGraph)
};

//==============================================================================
/** One band's controls under the graph: On, Type, Freq, Gain, Q, Slope (the cuts),
    Dynamic with its Threshold, Attack and Release, and LISTEN (held). Knobs a type
    doesn't use are dimmed (they stay editable). */
class EqBandEditor : public juce::Component
{
public:
    EqBandEditor (DrumFoundryProcessor& processor, int band);

    /** Re-reads the type and Dynamic, and dims what they don't use. */
    void update();
    int getBand() const noexcept { return band; }
    juce::Button& getListenButton() noexcept { return listen; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    DrumFoundryProcessor& processor;
    const int band;
    ToggleSwitch on;
    ChoiceSelector type, slope;
    Knob freq, gain, q, threshold, attack, release;
    ToggleSwitch dynamic;
    juce::TextButton listen { "LISTEN" };
    int shownType = -1, shownDynamic = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqBandEditor)
};

//==============================================================================
/** The full-screen EQ, over the whole editor (the EQ section's expand button opens it).
    Top: the EQ's switch, its Style, the graph's range, FIND PEAKS and close. Then a tab
    per band (its number and type, lit in its colour while it's on; double-click turns it
    on or off), the graph, and the selected band's controls. Esc closes it. */
class EqOverlay : public juce::Component, private juce::Timer
{
public:
    explicit EqOverlay (DrumFoundryProcessor& processor);
    ~EqOverlay() override;

    std::function<void()> onClose;

    void setHitAudio (const juce::AudioBuffer<float>& hit, double sampleRate) { graph.setHitAudio (hit, sampleRate); }
    EqGraph& getGraph() noexcept { return graph; }
    EqBandEditor& getBandEditor (int band) { return *editors[(size_t) juce::jlimit (0, kNumEqBands - 1, band)]; }
    juce::Button& getFindPeaksButton() noexcept { return findPeaks; }
    juce::Button& getBandTab (int band);
    void selectBand (int band);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

private:
    class BandTab;
    void timerCallback() override;
    juce::Rectangle<int> boxBounds() const;

    DrumFoundryProcessor& processor;
    PowerButton power;
    ChoiceSelector style;
    juce::TextButton range { "24 dB" }, findPeaks { "FIND PEAKS" }, closeButton { "CLOSE" };
    std::array<std::unique_ptr<BandTab>, kNumEqBands> bandTabs;
    EqGraph graph;
    std::array<std::unique_ptr<EqBandEditor>, kNumEqBands> editors;
    juce::Rectangle<int> titleArea, hintArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqOverlay)
};

} // namespace drum::ui
