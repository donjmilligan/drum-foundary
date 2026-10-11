// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"

namespace drum::ui
{

/** Browser preferences shared by every SampleBrowser and SampleSlotWidget in the
    process: the place and folder last shown, auto-preview, a folder the user
    chose, and the folder last imported from. Stored in
    ~/Library/Application Support/Drum Foundry/Browser.settings. Hold one
    through a juce::SharedResourcePointer. Message thread only. */
struct BrowserSettings
{
    BrowserSettings();

    juce::PropertiesFile file;
};

/** Sample browser panel (about 260-320 px wide, any height): a places menu,
    folder navigation, a recursive search of the place with quick filters, and a
    file list. Single-click (or up / down) auditions a file through the plug-in,
    double-click or Enter loads it into the target slot, and files can be dragged
    onto a SampleSlotWidget or out to the DAW / Finder.

    Folder listing and search run on a background thread. */
class SampleBrowser : public juce::Component,
                      public juce::DragAndDropContainer,
                      private juce::ListBoxModel,
                      private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    explicit SampleBrowser (DrumFoundryProcessor& processor);
    ~SampleBrowser() override;

    /** The slot a double-click / Enter / LOAD loads into. */
    void setTargetSlot (drum::SampleSlot slot);
    drum::SampleSlot getTargetSlot() const noexcept { return target; }

    /** Called after a file from the browser was handed to the processor to load. */
    std::function<void (drum::SampleSlot, const juce::File&)> onFileLoaded;
    /** Called when the user picks a different target slot in the browser. */
    std::function<void (drum::SampleSlot)> onTargetSlotChanged;

    /** Shows `folder` (as a custom place if it isn't inside one of the places). */
    void showFolder (const juce::File& folder);
    /** Sets the search text, as if typed. Empty goes back to the folder listing. */
    void setSearchText (const juce::String& text);

    /** Waits for the listing / search in progress to finish and shows it. For the
        dev tool, which has no message loop. Returns false on timeout. */
    bool waitForScan (int timeoutMs);

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&,
                                               juce::StringArray& files, bool& canMoveFiles) override;

private:
    class Scanner;

    struct Item
    {
        juce::File file;
        juce::String name;
        juce::String detail; // search results: the folder it's in, relative to the place
        bool isFolder = false;
        bool nameMatch = false; // search results: every word is in the file name itself
    };

    struct Place
    {
        juce::String name;
        juce::File folder;
    };

    // ListBoxModel
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void returnKeyPressed (int lastRowSelected) override { activate (lastRowSelected); }
    void deleteKeyPressed (int) override { goUp(); }
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    juce::String getTooltipForRow (int row) override;

    void handleAsyncUpdate() override;
    void timerCallback() override;

    void buildPlaces();
    void placeChosen();
    void chooseFolder();
    void setRoot (const juce::File& root, const juce::File& folder);
    void navigateTo (const juce::File& folder);
    void goUp();
    void startScan();
    void activate (int row);
    void preview (int row);
    void loadRow (int row);
    const Item* getItem (int row) const;
    const Item* getSelectedFile() const;
    void updateControls();
    juce::String breadcrumb (float width) const;

    DrumFoundryProcessor& processor;
    juce::SharedResourcePointer<BrowserSettings> settings;
    std::unique_ptr<Scanner> scanner;

    std::vector<Place> places;
    int placeIndex = 0;
    juce::File customFolder;
    juce::File root, current;
    juce::File selectAfterScan; // after going up: the folder we came from
    std::vector<Item> items;
    bool searchMode = false;
    juce::String status;
    int shownRequest = 0;
    juce::uint32 searchDue = 0; // debounce: when to start the typed search (0 = none)

    drum::SampleSlot target = drum::slotBody;
    juce::File previewing;
    bool wasPreviewing = false;
    bool suppressPreview = false;

    juce::ComboBox placeBox, targetBox;
    juce::TextButton upButton { "<" }, clearSearchButton { "X" };
    juce::TextEditor searchBox;
    std::array<juce::TextButton, 4> chips;
    juce::ListBox list;
    juce::TextButton autoButton { "AUTO" }, playButton { "PLAY" }, loadButton { "LOAD" };
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> titleArea, pathArea, statusArea, targetLabelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBrowser)
};

} // namespace drum::ui
