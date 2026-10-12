// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"
#include "../dsp/Reverb.h"

#include <atomic>
#include <mutex>

namespace drum::ui
{

/** The reverb's response as it's set: its impulse response (wet only, Mix at 100%),
    rendered on a background thread whenever a reverb setting changes, drawn as its
    waveform with its level envelope (dB) over it. Markers: the pre-delay, the gate's
    cut and fade (when it's on), and the slope Decay asks for (60 dB in Decay seconds)
    next to what the reverb does (the measured RT60 is in the corner). */
class ReverbResponse : public juce::Component, private juce::Timer, private juce::Thread
{
public:
    explicit ReverbResponse (DrumFoundryProcessor& processor);
    ~ReverbResponse() override;

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

    /** The last render's measured RT60 (s), or 0 before one is done (for tests). */
    float getMeasuredRt60() const noexcept { return measuredRt60.load(); }
    /** True once a render matching the current settings is on show (for tests). */
    bool isUpToDate() const noexcept;
    /** Renders the current settings now, on this thread (for tests and screenshots). */
    void renderNow();

private:
    struct Render
    {
        std::vector<float> peak;   // per 1 ms: the larger channel's peak, linear
        std::vector<float> lo, hi; // per 1 ms: lowest / highest sample (waveform)
        float rt60 = 0.0f;
        DrumParams params;
    };

    void timerCallback() override;
    void run() override;
    static std::shared_ptr<Render> makeRender (const DrumParams& p, drum::Reverb& reverb,
                                               const std::function<bool()>& shouldStop);
    static bool sameReverb (const DrumParams& a, const DrumParams& b) noexcept;

    DrumFoundryProcessor& processor;
    std::mutex lock;
    DrumParams wanted, shown;
    bool haveWanted = false;
    std::shared_ptr<const Render> render; // message thread only (set by the render's callAsync)
    std::atomic<float> measuredRt60 { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbResponse)
};

/** The reverb over the whole window (FULL on the Reverb section): its response, the
    engine (Cloud or Classic), and every control, grouped Space, Tone, Motion, Gate and
    Mix. The Cloud engine's own controls are dimmed while the engine is Classic. Esc,
    CLOSE or a click outside the box closes it. */
class ReverbOverlay : public juce::Component, private juce::Timer
{
public:
    explicit ReverbOverlay (DrumFoundryProcessor& processor);
    ~ReverbOverlay() override;

    std::function<void()> onClose;

    ReverbResponse& getResponse() noexcept { return response; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

private:
    struct Group
    {
        juce::String title;
        std::vector<juce::Component*> items;
        juce::Rectangle<int> area;
    };

    void timerCallback() override;
    juce::Rectangle<int> boxBounds() const;
    Knob& knob (const juce::String& id, const char* caption);

    DrumFoundryProcessor& processor;
    PowerButton power;
    ChoiceSelector engine;
    juce::TextButton closeButton { "CLOSE" };
    ReverbResponse response;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<ToggleSwitch> gateSwitch;
    std::vector<Group> groups;
    std::vector<juce::Component*> cloudOnly, notClassic; // dimmed on the other engines
    juce::Rectangle<int> titleArea, creditArea, engineLabelArea;
    int lastEngine = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbOverlay)
};

} // namespace drum::ui
