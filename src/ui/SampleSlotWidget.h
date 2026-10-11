// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "SampleBrowser.h"
#include "FoundryLookAndFeel.h"

namespace drum::ui
{

/** Compact strip (30-40 px tall) for one sample slot, to sit in a layer's panel:
    a thumbnail and the sample's name ("No sample", or "Missing: name" in red),
    IMPORT (file chooser), BROWSE (onBrowse) and a clear button. Audio files can be
    dropped on it from Finder or from a SampleBrowser. Clicking the name plays
    the loaded sample. It follows the processor by itself (sample generation). */
class SampleSlotWidget : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         public juce::DragAndDropTarget,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    SampleSlotWidget (DrumFoundryProcessor& processor, drum::SampleSlot slot, juce::Colour accent = colours::body);
    ~SampleSlotWidget() override;

    drum::SampleSlot getSlot() const noexcept { return slot; }

    /** BROWSE was clicked: show the browser and point it at this slot. */
    std::function<void (drum::SampleSlot)> onBrowse;

    /** Re-reads the slot now. It also does this by itself whenever the samples change. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    // Files from Finder
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { setDragOver (true); }
    void fileDragExit (const juce::StringArray&) override              { setDragOver (false); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // Files from a SampleBrowser (the description is the file's full path)
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { setDragOver (true); }
    void itemDragExit (const SourceDetails&) override  { setDragOver (false); }
    void itemDropped (const SourceDetails&) override;

private:
    void timerCallback() override;
    void setDragOver (bool shouldBeOver);
    void loadFile (const juce::File& file);
    void importFile();

    DrumFoundryProcessor& processor;
    const drum::SampleSlot slot;
    const juce::Colour accent;
    juce::SharedResourcePointer<BrowserSettings> settings;

    int shownGeneration = -1;
    bool loading = false, missing = false, dragOver = false;
    juce::String name, path;
    std::vector<std::pair<float, float>> peaks; // thumbnail, min / max per column

    juce::TextButton importButton { "IMPORT" }, browseButton { "BROWSE" }, clearButton;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> displayArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleSlotWidget)
};

} // namespace drum::ui
