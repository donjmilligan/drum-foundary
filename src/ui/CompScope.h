// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace drum::ui
{

/** What a compressor does to the hit, drawn from the latest render: the waveform going
    in (grey), the waveform coming out (in the compressor's colour, over it), and the gain
    reduction as a trace hanging from the top (its scale on the right). Hovering reads out
    the reduction at that moment. With nothing captured (the compressor is off, or not
    in the chain) it says so. */
class CompScope : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CompScope (juce::Colour accent);

    /** A capture from renderHit() (CompCaptures: input, output, gain change in dB), at
        `sampleRate`. An empty buffer means the compressor isn't working. */
    void setCapture (const juce::AudioBuffer<float>& capture, double sampleRate);

    /** The most reduction in the capture (dB, <= 0). */
    float getMaxReductionDb() const noexcept { return maxReductionDb; }
    bool hasCapture() const noexcept { return ! columns.empty(); }
    /** The reduction (dB) shown at x (component coordinates). */
    float getReductionAtX (float x) const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override { rebuild(); }
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    struct Column
    {
        float inLo = 0, inHi = 0, outLo = 0, outHi = 0, grDb = 0;
    };

    void rebuild();
    juce::Rectangle<float> plotArea() const;

    juce::Colour accent;
    juce::AudioBuffer<float> data;
    double rate = 48000.0;
    std::vector<Column> columns;
    float maxReductionDb = 0.0f, peak = 0.0f;
    float hoverX = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompScope)
};

} // namespace drum::ui
