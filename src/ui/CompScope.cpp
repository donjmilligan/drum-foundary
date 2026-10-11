// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "CompScope.h"
#include "FoundryLookAndFeel.h"

namespace drum::ui
{

namespace
{
    const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    juce::String formatDb (float db)
    {
        return std::abs (db) < 0.05f ? juce::String ("0 dB") : minusSign + juce::String (std::abs (db), 1) + " dB";
    }
}

CompScope::CompScope (juce::Colour a) : accent (a)
{
    setTooltip ("The hit through this compressor: grey going in, coloured coming out, and the gain reduction hanging from the top.");
}

void CompScope::setCapture (const juce::AudioBuffer<float>& capture, double sampleRate)
{
    if (capture.getNumChannels() >= 3 && capture.getNumSamples() > 0)
        data.makeCopyOf (capture);
    else
        data.setSize (3, 0);
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    rebuild();
}

juce::Rectangle<float> CompScope::plotArea() const
{
    return getLocalBounds().toFloat().reduced (6.0f, 4.0f).withTrimmedRight (34.0f);
}

void CompScope::rebuild()
{
    columns.clear();
    maxReductionDb = 0.0f;
    peak = 0.0f;
    const int n = data.getNumSamples();
    const auto plot = plotArea();
    const int width = juce::jmax (1, (int) plot.getWidth());
    if (n == 0 || plot.isEmpty())
    {
        repaint();
        return;
    }

    const float* in = data.getReadPointer (0);
    const float* out = data.getReadPointer (1);
    const float* gr = data.getReadPointer (2);
    columns.resize ((size_t) width);
    for (int x = 0; x < width; ++x)
    {
        const int a = (int) ((int64_t) x * n / width), b = juce::jmax (a + 1, (int) ((int64_t) (x + 1) * n / width));
        Column c { 1e9f, -1e9f, 1e9f, -1e9f, 0.0f };
        for (int i = a; i < juce::jmin (b, n); ++i)
        {
            c.inLo = juce::jmin (c.inLo, in[i]);
            c.inHi = juce::jmax (c.inHi, in[i]);
            c.outLo = juce::jmin (c.outLo, out[i]);
            c.outHi = juce::jmax (c.outHi, out[i]);
            c.grDb = juce::jmin (c.grDb, gr[i]);
        }
        peak = juce::jmax (peak, juce::jmax (std::abs (c.inLo), std::abs (c.inHi), std::abs (c.outLo), std::abs (c.outHi)));
        maxReductionDb = juce::jmin (maxReductionDb, c.grDb);
        columns[(size_t) x] = c;
    }
    repaint();
}

float CompScope::getReductionAtX (float x) const noexcept
{
    if (columns.empty())
        return 0.0f;
    const auto plot = plotArea();
    const int i = juce::jlimit (0, (int) columns.size() - 1, (int) (x - plot.getX()));
    return columns[(size_t) i].grDb;
}

void CompScope::mouseMove (const juce::MouseEvent& e)
{
    hoverX = e.position.x;
    repaint();
}

void CompScope::mouseExit (const juce::MouseEvent&)
{
    hoverX = -1.0f;
    repaint();
}

void CompScope::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto plot = plotArea();
    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 4.0f);

    if (columns.empty())
    {
        g.setFont (uiFont (11.0f));
        g.setColour (colours::textDim);
        g.drawText ("Switch the compressor on (and put it in the chain) to see what it does to the hit",
                    bounds.reduced (8.0f), juce::Justification::centred, true);
        return;
    }

    // The gain reduction's scale: at least 6 dB, rounded up to 6 dB steps.
    const float range = juce::jmax (6.0f, 6.0f * std::ceil (-maxReductionDb / 6.0f));
    const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f;
    const float scale = peak > 1.0e-6f ? half / peak : 1.0f;

    g.setColour (colours::grid);
    g.drawHorizontalLine (juce::roundToInt (mid), plot.getX(), plot.getRight());

    // In (grey), then out (colour) over it.
    for (int pass = 0; pass < 2; ++pass)
    {
        juce::Path p;
        for (size_t x = 0; x < columns.size(); ++x)
        {
            const auto& c = columns[x];
            const float lo = pass == 0 ? c.inLo : c.outLo, hi = pass == 0 ? c.inHi : c.outHi;
            const float px = plot.getX() + (float) x;
            p.addRectangle (px, mid - hi * scale, 1.0f, juce::jmax (1.0f, (hi - lo) * scale));
        }
        g.setColour (pass == 0 ? colours::textDim.withAlpha (0.35f) : accent.withAlpha (0.75f));
        g.fillPath (p);
    }

    // The reduction, hanging from the top.
    {
        juce::Path fill, line;
        fill.startNewSubPath (plot.getX(), plot.getY());
        for (size_t x = 0; x < columns.size(); ++x)
        {
            const float y = plot.getY() + (-columns[x].grDb / range) * plot.getHeight();
            const float px = plot.getX() + (float) x;
            fill.lineTo (px, y);
            if (x == 0) line.startNewSubPath (px, y); else line.lineTo (px, y);
        }
        fill.lineTo (plot.getX() + (float) columns.size(), plot.getY());
        fill.closeSubPath();
        g.setColour (colours::clipZone.withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (colours::clipZone.withAlpha (0.95f));
        g.strokePath (line, juce::PathStrokeType (1.4f));
    }

    // Its scale, right of the plot.
    g.setFont (uiFont (9.5f));
    g.setColour (colours::clipZone.withAlpha (0.8f));
    for (float db : { 0.0f, -range * 0.5f, -range })
    {
        const float y = plot.getY() + (-db / range) * plot.getHeight();
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (plot.getRight() + 3.0f, y - 6.0f, 30.0f, 12.0f),
                    juce::Justification::centredLeft);
    }

    // Readout: the most reduction, or what's under the mouse.
    juce::String text;
    if (hoverX >= plot.getX() && hoverX <= plot.getRight())
    {
        const float ms = (hoverX - plot.getX()) / plot.getWidth() * (float) data.getNumSamples() / (float) rate * 1000.0f;
        text = juce::String (juce::roundToInt (ms)) + " ms   " + formatDb (getReductionAtX (hoverX));
        g.setColour (colours::text.withAlpha (0.4f));
        g.drawVerticalLine (juce::roundToInt (hoverX), plot.getY(), plot.getBottom());
    }
    else
    {
        text = "MAX GR " + formatDb (maxReductionDb);
    }
    g.setFont (uiFont (10.5f, true));
    g.setColour (colours::text.withAlpha (0.85f));
    g.drawText (text, plot.reduced (4.0f, 2.0f), juce::Justification::bottomRight);
}

} // namespace drum::ui
