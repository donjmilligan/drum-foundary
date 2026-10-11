// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "SampleSlotWidget.h"

namespace drum::ui
{

namespace
{
    constexpr int kThumbColumns = 48;
    const juce::Colour missingColour { 0xffff5a5a };
    const char* const importFolderKey = "importFolder";

    /** A dropped / dragged path, if it's an audio file we can read. */
    juce::File audioFileFrom (const juce::String& path)
    {
        if (! juce::File::isAbsolutePath (path))
            return {};
        const juce::File f (path);
        return f.existsAsFile() && SampleBank::isAudioFile (f) ? f : juce::File();
    }
}

SampleSlotWidget::SampleSlotWidget (DrumFoundryProcessor& p, SampleSlot s, juce::Colour accentColour)
    : processor (p), slot (s), accent (accentColour)
{
    importButton.setTooltip ("Load an audio file into " + sampleSlotName (slot));
    importButton.onClick = [this] { importFile(); };
    addAndMakeVisible (importButton);

    browseButton.setTooltip ("Find a sample for " + sampleSlotName (slot) + " in the browser");
    browseButton.onClick = [this] { if (onBrowse) onBrowse (slot); };
    addAndMakeVisible (browseButton);

    clearButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
    clearButton.setTooltip ("Remove the sample");
    clearButton.onClick = [this] { processor.clearSample (slot); };
    addAndMakeVisible (clearButton);

    refresh();
    startTimerHz (10);
}

SampleSlotWidget::~SampleSlotWidget()
{
    stopTimer();
}

//==============================================================================
void SampleSlotWidget::timerCallback()
{
    if (processor.getSampleGeneration() != shownGeneration)
        refresh();
}

void SampleSlotWidget::refresh()
{
    shownGeneration = processor.getSampleGeneration();

    const auto sample = processor.getSample (slot);
    path = processor.getSamplePath (slot);
    loading = processor.isSampleLoading (slot);
    missing = processor.isSampleMissing (slot);
    name = sample != nullptr ? sample->name
                             : (path.isNotEmpty() && juce::File::isAbsolutePath (path) ? juce::File (path).getFileNameWithoutExtension()
                                                                                         : juce::String());

    peaks.clear();
    if (sample != nullptr && sample->audio.getNumSamples() > 0)
    {
        const auto& audio = sample->audio;
        const int n = audio.getNumSamples();
        for (int col = 0; col < kThumbColumns; ++col)
        {
            const int start = (int) ((juce::int64) n * col / kThumbColumns);
            const int end = juce::jmax (start + 1, (int) ((juce::int64) n * (col + 1) / kThumbColumns));
            float lo = 0.0f, hi = 0.0f;
            for (int c = 0; c < audio.getNumChannels(); ++c)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (c, start), juce::jmin (end, n) - start);
                lo = juce::jmin (lo, range.getStart());
                hi = juce::jmax (hi, range.getEnd());
            }
            peaks.emplace_back (lo, hi);
        }

        // Normalise, so a quiet sample still reads as a shape.
        float peak = 0.0f;
        for (const auto& [lo, hi] : peaks)
            peak = juce::jmax (peak, -lo, hi);
        if (peak > 1.0e-6f)
            for (auto& [lo, hi] : peaks)
            {
                lo /= peak;
                hi /= peak;
            }

        setTooltip (path + "\n" + juce::String ((double) n / sample->sampleRate, 2) + " s, "
                    + juce::String (sample->sampleRate / 1000.0, 1) + " kHz, "
                    + (audio.getNumChannels() > 1 ? "stereo" : "mono") + ". Click to play.");
    }
    else if (missing)
    {
        setTooltip ("Can't find " + path + ". Drop the file here, or use IMPORT or BROWSE to replace it.");
    }
    else
    {
        setTooltip ("Drop an audio file here, or use IMPORT or BROWSE");
    }

    clearButton.setEnabled (path.isNotEmpty());
    repaint();
}

//==============================================================================
void SampleSlotWidget::paint (juce::Graphics& g)
{
    const auto box = displayArea.toFloat().reduced (0.5f);
    const bool hot = dragOver || (isMouseOver() && ! peaks.empty() && displayArea.contains (getMouseXYRelative()));

    g.setColour (dragOver ? accent.withAlpha (0.12f) : colours::display);
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (dragOver ? accent : (hot ? colours::textDim : colours::panelBorder));
    g.drawRoundedRectangle (box, 4.0f, dragOver ? 1.5f : 1.0f);

    auto inner = displayArea.reduced (6, 4);
    auto thumb = inner.removeFromLeft (juce::jmin (46, inner.getWidth() / 4)).toFloat();
    inner.removeFromLeft (inner.getWidth() < 120 ? 6 : 8);

    if (peaks.empty())
    {
        g.setColour (colours::track);
        g.fillRect (thumb.withSizeKeepingCentre (thumb.getWidth(), 1.0f));
    }
    else
    {
        const float colW = thumb.getWidth() / (float) peaks.size();
        const float mid = thumb.getCentreY(), half = thumb.getHeight() * 0.5f;
        g.setColour (accent.withAlpha (0.85f));
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const float top = mid - peaks[i].second * half;
            const float bottom = mid - peaks[i].first * half;
            g.fillRect (thumb.getX() + (float) i * colW, top, juce::jmax (1.0f, colW - 0.4f), juce::jmax (1.0f, bottom - top));
        }
    }

    juce::String text;
    juce::Colour colour = colours::text;
    if (dragOver)
    {
        text = "Drop to load";
        colour = accent;
    }
    else if (loading)
    {
        text = "Loading" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
        colour = colours::textDim;
    }
    else if (missing)
    {
        text = "Missing: " + name;
        colour = missingColour;
    }
    else if (name.isEmpty())
    {
        text = "No sample";
        colour = colours::textDim;
    }
    else
    {
        text = name;
    }

    g.setFont (uiFont (12.5f, ! peaks.empty() || missing));
    g.setColour (colour);
    g.drawText (text, inner, juce::Justification::centredLeft, true);
}

void SampleSlotWidget::resized()
{
    auto area = getLocalBounds();
    const int buttonHeight = juce::jmin (24, area.getHeight());

    // Narrow (in a layer panel): tighter buttons, so the name keeps some room.
    const bool narrow = area.getWidth() < 340;
    auto place = [&] (juce::Button& b, int width)
    {
        b.setBounds (area.removeFromRight (width).withSizeKeepingCentre (width, buttonHeight));
        area.removeFromRight (narrow ? 4 : 5);
    };
    place (clearButton, narrow ? 22 : 24);
    place (browseButton, narrow ? 56 : 60);
    place (importButton, narrow ? 54 : 58);

    displayArea = area;
}

void SampleSlotWidget::mouseUp (const juce::MouseEvent& e)
{
    // Click the name to hear what's loaded.
    if (! displayArea.contains (e.getPosition()) || e.mouseWasDraggedSinceMouseDown() || e.mods.isPopupMenu())
        return;

    if (const auto file = audioFileFrom (path); file != juce::File() && ! missing)
        processor.previewFile (file);
}

//==============================================================================
void SampleSlotWidget::setDragOver (bool shouldBeOver)
{
    if (dragOver != shouldBeOver)
    {
        dragOver = shouldBeOver;
        repaint();
    }
}

void SampleSlotWidget::loadFile (const juce::File& file)
{
    setDragOver (false);
    if (file == juce::File())
        return;

    processor.loadSample (slot, file);
    refresh(); // shows "Loading..." straight away
}

bool SampleSlotWidget::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (audioFileFrom (f) != juce::File())
            return true;
    return false;
}

void SampleSlotWidget::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (const auto file = audioFileFrom (f); file != juce::File())
        {
            loadFile (file);
            return;
        }
    setDragOver (false);
}

bool SampleSlotWidget::isInterestedInDragSource (const SourceDetails& details)
{
    return audioFileFrom (details.description.toString()) != juce::File();
}

void SampleSlotWidget::itemDropped (const SourceDetails& details)
{
    loadFile (audioFileFrom (details.description.toString()));
}

void SampleSlotWidget::importFile()
{
    auto& props = settings->file;
    auto folder = juce::File::isAbsolutePath (props.getValue (importFolderKey)) ? juce::File (props.getValue (importFolderKey))
                                                                               : juce::File();
    if (const auto current = audioFileFrom (path); current != juce::File())
        folder = current.getParentDirectory();
    if (! folder.isDirectory())
        folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Import a sample into " + sampleSlotName (slot), folder,
                                                   SampleBank::audioFileWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (! file.existsAsFile())
                                  return;

                              settings->file.setValue (importFolderKey, file.getParentDirectory().getFullPathName());
                              loadFile (file);
                          });
}

} // namespace drum::ui
