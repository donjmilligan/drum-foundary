// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "SampleBrowser.h"
#include "FoundryLookAndFeel.h"

#include <optional>

namespace drum::ui
{

namespace
{
    constexpr int kChooseFolderId = 1000;
    constexpr int kFolderRowHeight = 24;
    constexpr int kSearchRowHeight = 34;
    constexpr int kSearchDelayMs = 250;
    const char* const kChipNames[] { "Kick", "Snare", "Clap", "Hat" };

    namespace keys
    {
        const char* const place        = "place";
        const char* const folder       = "folder";
        const char* const customFolder = "customFolder";
        const char* const autoPreview  = "autoPreview";
    }

    const juce::String ellipsis (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
    const juce::String cross (juce::CharPointer_UTF8 ("\xc3\x97"));

    /** A saved path, or {} if it isn't a usable absolute path. */
    juce::File fileFromSetting (const juce::PropertiesFile& props, const char* key)
    {
        const auto path = props.getValue (key);
        return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    }

    bool isInside (const juce::File& f, const juce::File& folder)
    {
        return f == folder || f.isAChildOf (folder);
    }

    /** Five little bars, the same glyph as the DRAG button. */
    void drawWaveGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        static constexpr float bars[] { 0.35f, 1.0f, 0.7f, 0.45f, 0.25f };
        const float bw = area.getWidth() / (float) std::size (bars);
        g.setColour (colour);
        for (size_t i = 0; i < std::size (bars); ++i)
        {
            const float h = area.getHeight() * bars[i];
            g.fillRoundedRectangle (area.getX() + (float) i * bw + 0.5f, area.getCentreY() - h * 0.5f, bw - 1.2f, h, 0.6f);
        }
    }

    void drawFolderGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        const auto r = area.withSizeKeepingCentre (area.getWidth(), area.getWidth() * 0.78f);
        juce::Path p;
        p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth() * 0.45f, r.getHeight() * 0.4f, 1.0f);
        p.addRoundedRectangle (r.withTrimmedTop (r.getHeight() * 0.2f), 1.5f);
        g.setColour (colour);
        g.fillPath (p);
    }

    /** Live's metadata folders, which never hold samples. */
    bool isHelperFolder (const juce::File& folder)
    {
        const auto name = folder.getFileName();
        return name == "Ableton Folder Info" || name == "Ableton Project Info";
    }

    /** `parts` joined with " / ", dropping parts from the left until it fits `width`. */
    juce::String fitPath (const juce::StringArray& parts, const juce::Font& font, float width)
    {
        auto text = parts.joinIntoString (" / ");
        for (int first = 1; first < parts.size() && juce::GlyphArrangement::getStringWidth (font, text) > width; ++first)
            text = ellipsis + " / " + parts.joinIntoString (" / ", first);
        return text;
    }

    void drawPlayGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        const auto r = area.reduced (area.getWidth() * 0.15f);
        juce::Path tri;
        tri.addTriangle (r.getX(), r.getY(), r.getX(), r.getBottom(), r.getRight(), r.getCentreY());
        g.setColour (colour);
        g.fillPath (tri);
    }
}

//==============================================================================
BrowserSettings::BrowserSettings()
    : file ([]
            {
                juce::PropertiesFile::Options o;
                o.applicationName = "Browser";
                o.folderName = "Drum Foundry";
                o.filenameSuffix = ".settings";
                o.osxLibrarySubFolder = "Application Support";
                o.millisecondsBeforeSaving = 1000;
                return o;
            }())
{
}

//==============================================================================
/** Lists a folder, or searches a place recursively, on its own thread. Only the
    newest request counts: an older one stops as soon as a new one arrives. A
    search posts what it has found so far every so often, so results appear
    while it's still going. */
class SampleBrowser::Scanner : private juce::Thread
{
public:
    static constexpr int maxResults = 500;

    struct Result
    {
        int request = 0;
        std::vector<Item> items;
        bool search = false, finished = false, capped = false, folderMissing = false;
    };

    explicit Scanner (juce::AsyncUpdater& o) : juce::Thread ("Drum Foundry browser"), owner (o)
    {
        startThread (juce::Thread::Priority::low);
    }

    ~Scanner() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    int list (const juce::File& folder)
    {
        return post ({ 0, false, folder, {} });
    }

    int search (const juce::File& placeRoot, const juce::String& query)
    {
        auto tokens = juce::StringArray::fromTokens (query.toLowerCase(), true);
        tokens.removeEmptyStrings();
        return post ({ 0, true, placeRoot, tokens });
    }

    std::optional<Result> take()
    {
        const juce::ScopedLock sl (lock);
        return std::exchange (result, std::nullopt);
    }

    bool isIdle() const
    {
        const juce::ScopedLock sl (lock);
        return ! pending.has_value() && ! busy;
    }

private:
    struct Request
    {
        int id = 0;
        bool search = false;
        juce::File folder;
        juce::StringArray tokens;
    };

    int post (Request r)
    {
        {
            const juce::ScopedLock sl (lock);
            r.id = ++latest;
            pending = std::move (r);
        }
        notify();
        return latest.load();
    }

    bool cancelled (int id) const { return threadShouldExit() || latest.load() != id; }

    void publish (Result r)
    {
        {
            const juce::ScopedLock sl (lock);
            result = std::move (r);
        }
        owner.triggerAsyncUpdate();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            std::optional<Request> request;
            {
                const juce::ScopedLock sl (lock);
                request = std::exchange (pending, std::nullopt);
                busy = request.has_value();
            }

            if (! request.has_value())
            {
                wait (-1);
                continue;
            }

            if (request->search)
                runSearch (*request);
            else
                runList (*request);

            const juce::ScopedLock sl (lock);
            busy = false;
        }
    }

    void runList (const Request& r)
    {
        Result res;
        res.request = r.id;
        res.finished = true;

        if (! r.folder.isDirectory())
        {
            res.folderMissing = true;
            publish (std::move (res));
            return;
        }

        for (const auto& f : r.folder.findChildFiles (juce::File::findDirectories | juce::File::ignoreHiddenFiles, false))
            if (! isHelperFolder (f))
                res.items.push_back ({ f, f.getFileName(), {}, true });

        for (const auto& f : r.folder.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false))
            if (SampleBank::isAudioFile (f))
                res.items.push_back ({ f, f.getFileNameWithoutExtension(), {}, false });

        if (cancelled (r.id))
            return;

        std::sort (res.items.begin(), res.items.end(), [] (const Item& a, const Item& b)
        {
            if (a.isFolder != b.isFolder)
                return a.isFolder;
            return a.file.getFileName().compareNatural (b.file.getFileName()) < 0;
        });
        publish (std::move (res));
    }

    void runSearch (const Request& r)
    {
        Result res;
        res.request = r.id;
        res.search = true;

        const auto sortedCopy = [&res]
        {
            auto copy = res;
            std::sort (copy.items.begin(), copy.items.end(), [] (const Item& a, const Item& b)
            {
                if (a.nameMatch != b.nameMatch)
                    return a.nameMatch;
                if (const int c = a.detail.compareNatural (b.detail); c != 0)
                    return c < 0;
                return a.name.compareNatural (b.name) < 0;
            });
            return copy;
        };

        auto lastPost = juce::Time::getMillisecondCounter();
        size_t lastPostedCount = 0;

        if (r.folder.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (r.folder, true, "*",
                                                                    juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (cancelled (r.id))
                    return;

                const auto& f = entry.getFile();
                if (! SampleBank::isAudioFile (f))
                    continue;

                // Every word must appear somewhere in the path below the place, so
                // "kick" also finds "Drums/Kicks/BD 01.wav".
                const auto relative = f.getRelativePathFrom (r.folder);
                const auto haystack = relative.toLowerCase();
                bool match = true;
                for (const auto& t : r.tokens)
                    match = match && haystack.contains (t);
                if (! match)
                    continue;

                // Files whose own name matches come first.
                const auto name = f.getFileNameWithoutExtension();
                const auto lowerName = name.toLowerCase();
                bool nameMatch = true;
                for (const auto& t : r.tokens)
                    nameMatch = nameMatch && lowerName.contains (t);

                const auto parent = f.getParentDirectory();
                res.items.push_back ({ f, name,
                                       parent == r.folder ? r.folder.getFileName() : parent.getRelativePathFrom (r.folder),
                                       false, nameMatch });

                if ((int) res.items.size() >= maxResults)
                {
                    res.capped = true;
                    break;
                }

                const auto now = juce::Time::getMillisecondCounter();
                if (now - lastPost > 150 && res.items.size() != lastPostedCount)
                {
                    lastPost = now;
                    lastPostedCount = res.items.size();
                    publish (sortedCopy());
                }
            }
        }

        if (cancelled (r.id))
            return;

        res.finished = true;
        publish (sortedCopy());
    }

    juce::AsyncUpdater& owner;
    mutable juce::CriticalSection lock;
    std::optional<Request> pending;
    std::optional<Result> result;
    bool busy = false;
    std::atomic<int> latest { 0 };
};

//==============================================================================
SampleBrowser::SampleBrowser (DrumFoundryProcessor& p)
    : processor (p), scanner (std::make_unique<Scanner> (static_cast<juce::AsyncUpdater&> (*this)))
{
    auto& props = settings->file;
    customFolder = fileFromSetting (props, keys::customFolder);

    placeBox.setTooltip ("Where to browse");
    placeBox.onChange = [this] { placeChosen(); };
    addAndMakeVisible (placeBox);

    upButton.setTooltip ("Up one folder");
    upButton.onClick = [this] { goUp(); };
    addAndMakeVisible (upButton);

    searchBox.setFont (uiFont (13.0f));
    searchBox.setIndents (8, 5);
    searchBox.setTextToShowWhenEmpty ("Search this place", colours::textDim);
    searchBox.setTooltip ("Search every folder in this place (all words must match the path)");
    searchBox.onTextChange = [this]
    {
        searchDue = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter() + (juce::uint32) kSearchDelayMs);
        updateControls();
    };
    searchBox.onReturnKey = [this]
    {
        if (searchDue != 0)
        {
            searchDue = 0;
            startScan();
        }
        list.grabKeyboardFocus();
        if (! items.empty() && list.getSelectedRows().isEmpty())
            list.selectRow (0);
    };
    searchBox.onEscapeKey = [this] { setSearchText ({}); };
    addAndMakeVisible (searchBox);

    clearSearchButton.setButtonText (cross);
    clearSearchButton.setTooltip ("Clear the search");
    clearSearchButton.onClick = [this] { setSearchText ({}); };
    addChildComponent (clearSearchButton);

    for (size_t i = 0; i < chips.size(); ++i)
    {
        auto& chip = chips[i];
        const juce::String word (kChipNames[i]);
        chip.setButtonText (word.toUpperCase());
        chip.setTooltip ("Search for \"" + word + "\"");
        chip.setColour (juce::TextButton::buttonOnColourId, colours::body);
        chip.onClick = [this, word]
        {
            setSearchText (searchBox.getText().trim().equalsIgnoreCase (word) ? juce::String() : word);
        };
        addAndMakeVisible (chip);
    }

    list.setModel (this);
    list.setRowHeight (kFolderRowHeight);
    list.setColour (juce::ListBox::backgroundColourId, colours::display);
    list.setColour (juce::ListBox::outlineColourId, colours::panelBorder);
    list.setOutlineThickness (1);
    list.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, colours::track.brighter (0.35f));
    addAndMakeVisible (list);

    autoButton.setClickingTogglesState (true);
    autoButton.setToggleState (props.getBoolValue (keys::autoPreview, true), juce::dontSendNotification);
    autoButton.setColour (juce::TextButton::buttonOnColourId, colours::body);
    autoButton.setTooltip ("Auto-preview: play each file as you select it");
    autoButton.onClick = [this] { settings->file.setValue (keys::autoPreview, autoButton.getToggleState()); };
    addAndMakeVisible (autoButton);

    playButton.setTooltip ("Play the selected file (Space)");
    playButton.onClick = [this]
    {
        if (processor.isPreviewing())
            processor.stopPreview();
        else if (getSelectedFile() != nullptr)
            preview (list.getSelectedRow());
    };
    addAndMakeVisible (playButton);

    for (int s = 0; s < numSampleSlots; ++s)
        targetBox.addItem (sampleSlotName ((SampleSlot) s), s + 1);
    targetBox.setSelectedId (target + 1, juce::dontSendNotification);
    targetBox.setColour (juce::ComboBox::textColourId, colours::body);
    targetBox.setTooltip ("The layer that double-click, Enter and LOAD put the sample in");
    targetBox.onChange = [this]
    {
        if (const int id = targetBox.getSelectedId(); id > 0)
        {
            target = (SampleSlot) (id - 1);
            if (onTargetSlotChanged)
                onTargetSlotChanged (target);
        }
    };
    addAndMakeVisible (targetBox);

    loadButton.setTooltip ("Load the selected file into the target layer (double-click / Enter)");
    loadButton.onClick = [this] { loadRow (list.getSelectedRow()); };
    addAndMakeVisible (loadButton);

    // Where we were last time, else the User Library (or Music if there's no Live).
    buildPlaces();
    const auto savedPlace = props.getValue (keys::place);
    int index = -1;
    for (int i = 0; i < (int) places.size(); ++i)
        if (places[(size_t) i].name == savedPlace && places[(size_t) i].folder.isDirectory())
            index = i;
    if (index < 0)
        index = places[0].folder.isDirectory() ? 0 : 2;

    placeIndex = index;
    placeBox.setSelectedId (index + 1, juce::dontSendNotification);
    setRoot (places[(size_t) index].folder, fileFromSetting (props, keys::folder));

    startTimerHz (20);
}

SampleBrowser::~SampleBrowser()
{
    stopTimer();
    scanner = nullptr; // joined: nothing posts results from here on
    cancelPendingUpdate();

    if (processor.isPreviewing() && previewing != juce::File())
        processor.stopPreview();

    list.setModel (nullptr);
}

//==============================================================================
void SampleBrowser::buildPlaces()
{
    const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    // Live keeps its library in ~/Music/Ableton on macOS and in Documents\Ableton on Windows.
   #if JUCE_WINDOWS
    const auto ableton = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Ableton");
   #else
    const auto ableton = music.getChildFile ("Ableton");
   #endif

    auto userLibrary = ableton.getChildFile ("User Library/Samples");
    if (! userLibrary.isDirectory())
        userLibrary = ableton.getChildFile ("User Library");

    places = { { "Ableton User Library",  userLibrary },
               { "Ableton Factory Packs", ableton.getChildFile ("Factory Packs") },
               { "Music",                 music },
               { "Desktop",               juce::File::getSpecialLocation (juce::File::userDesktopDirectory) },
               { "Downloads",             home.getChildFile ("Downloads") } };

    if (customFolder.isDirectory())
        places.push_back ({ customFolder.getFileName(), customFolder });

    placeBox.clear (juce::dontSendNotification);
    for (int i = 0; i < (int) places.size(); ++i)
    {
        placeBox.addItem (places[(size_t) i].name, i + 1);
        placeBox.setItemEnabled (i + 1, places[(size_t) i].folder.isDirectory());
    }
    placeBox.addSeparator();
    placeBox.addItem ("Choose Folder" + ellipsis, kChooseFolderId);
}

void SampleBrowser::placeChosen()
{
    const int id = placeBox.getSelectedId();
    if (id == kChooseFolderId)
    {
        placeBox.setSelectedId (placeIndex + 1, juce::dontSendNotification);
        chooseFolder();
        return;
    }

    if (! juce::isPositiveAndBelow (id - 1, (int) places.size()))
        return;

    placeIndex = id - 1;
    const auto& place = places[(size_t) placeIndex];
    settings->file.setValue (keys::place, place.name);
    setRoot (place.folder, place.folder);
}

void SampleBrowser::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a folder of samples", current.isDirectory() ? current : root);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();
                              if (folder.isDirectory())
                                  showFolder (folder);
                          });
}

void SampleBrowser::showFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return;

    // The most specific place that contains it; failing that, it becomes the custom place.
    int index = -1;
    for (int i = 0; i < (int) places.size(); ++i)
        if (places[(size_t) i].folder.isDirectory() && isInside (folder, places[(size_t) i].folder)
            && (index < 0 || places[(size_t) i].folder.isAChildOf (places[(size_t) index].folder)))
            index = i;

    if (index < 0)
    {
        customFolder = folder;
        settings->file.setValue (keys::customFolder, folder.getFullPathName());
        buildPlaces();
        index = (int) places.size() - 1;
    }

    placeIndex = index;
    placeBox.setSelectedId (index + 1, juce::dontSendNotification);
    settings->file.setValue (keys::place, places[(size_t) index].name);
    setRoot (places[(size_t) index].folder, folder);
}

void SampleBrowser::setRoot (const juce::File& newRoot, const juce::File& folder)
{
    root = newRoot;
    navigateTo (folder.isDirectory() && isInside (folder, root) ? folder : root);
}

void SampleBrowser::navigateTo (const juce::File& folder)
{
    current = folder;
    settings->file.setValue (keys::folder, folder.getFullPathName());
    searchBox.setText ({}, juce::dontSendNotification);
    searchDue = 0;
    startScan();
}

void SampleBrowser::goUp()
{
    if (searchMode || searchBox.getText().isNotEmpty())
    {
        setSearchText ({});
        return;
    }

    if (current.isAChildOf (root))
    {
        selectAfterScan = current;
        navigateTo (current.getParentDirectory());
    }
}

void SampleBrowser::setSearchText (const juce::String& text)
{
    searchBox.setText (text, juce::dontSendNotification);
    searchDue = 0;
    startScan();
}

void SampleBrowser::startScan()
{
    const auto query = searchBox.getText().trim();
    searchMode = query.isNotEmpty();
    shownRequest = searchMode ? scanner->search (root, query) : scanner->list (current);
    status = searchMode ? "Searching" + ellipsis : juce::String();

    suppressPreview = true;
    items.clear();
    list.setRowHeight (searchMode ? kSearchRowHeight : kFolderRowHeight);
    list.deselectAllRows();
    list.updateContent();
    list.scrollToEnsureRowIsOnscreen (0);
    suppressPreview = false;

    updateControls();
    repaint();
}

bool SampleBrowser::waitForScan (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    if (searchDue != 0)
    {
        searchDue = 0;
        startScan();
    }
    while (! scanner->isIdle())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::Thread::sleep (5);
    }
    handleUpdateNowIfNeeded();
    return true;
}

void SampleBrowser::handleAsyncUpdate()
{
    auto result = scanner->take();
    if (! result.has_value() || result->request != shownRequest)
        return;

    const auto* selected = getItem (list.getSelectedRow());
    const auto keep = selectAfterScan != juce::File() ? selectAfterScan : (selected != nullptr ? selected->file : juce::File());
    if (result->finished)
        selectAfterScan = juce::File();

    items = std::move (result->items);

    suppressPreview = true;
    list.updateContent();
    list.deselectAllRows();
    for (int i = 0; i < (int) items.size(); ++i)
        if (items[(size_t) i].file == keep)
        {
            list.selectRow (i);
            break;
        }
    suppressPreview = false;

    const int n = (int) items.size();
    if (result->folderMissing)
    {
        status = "Folder not found";
    }
    else if (result->search)
    {
        if (! result->finished)
            status = "Searching" + ellipsis + " " + juce::String (n);
        else if (result->capped)
            status = "First " + juce::String (n) + " matches";
        else
            status = n == 0 ? juce::String ("No matches") : juce::String (n) + (n == 1 ? " match" : " matches");
    }
    else
    {
        int folders = 0;
        for (const auto& item : items)
            folders += item.isFolder ? 1 : 0;
        const int files = n - folders;

        juce::StringArray parts;
        if (folders > 0)
            parts.add (juce::String (folders) + (folders == 1 ? " folder" : " folders"));
        if (files > 0)
            parts.add (juce::String (files) + (files == 1 ? " sample" : " samples"));
        status = parts.isEmpty() ? juce::String ("No samples here") : parts.joinIntoString (", ");
    }

    updateControls();
    repaint (statusArea);
}

void SampleBrowser::timerCallback()
{
    if (searchDue != 0 && juce::Time::getMillisecondCounter() >= searchDue)
    {
        searchDue = 0;
        startScan();
    }

    if (const bool playing = processor.isPreviewing(); playing != wasPreviewing)
    {
        wasPreviewing = playing;
        playButton.setButtonText (playing ? "STOP" : "PLAY");
        updateControls();
        list.repaint();
    }
}

//==============================================================================
void SampleBrowser::setTargetSlot (SampleSlot slot)
{
    if (! juce::isPositiveAndBelow ((int) slot, (int) numSampleSlots))
        return;

    target = slot;
    targetBox.setSelectedId (slot + 1, juce::dontSendNotification);
}

const SampleBrowser::Item* SampleBrowser::getItem (int row) const
{
    return juce::isPositiveAndBelow (row, (int) items.size()) ? &items[(size_t) row] : nullptr;
}

const SampleBrowser::Item* SampleBrowser::getSelectedFile() const
{
    const auto* item = getItem (list.getSelectedRow());
    return item != nullptr && ! item->isFolder ? item : nullptr;
}

void SampleBrowser::activate (int row)
{
    if (const auto* item = getItem (row))
    {
        if (item->isFolder)
            navigateTo (item->file);
        else
            loadRow (row);
    }
}

void SampleBrowser::preview (int row)
{
    const auto* item = getItem (row);
    if (item == nullptr || item->isFolder)
        return;

    previewing = item->file;
    processor.previewFile (item->file);
    list.repaint();
}

void SampleBrowser::loadRow (int row)
{
    const auto* item = getItem (row);
    if (item == nullptr || item->isFolder)
        return;

    const auto file = item->file;
    processor.loadSample (target, file);
    if (onFileLoaded)
        onFileLoaded (target, file);
}

void SampleBrowser::updateControls()
{
    const bool hasQuery = searchBox.getText().isNotEmpty();
    upButton.setEnabled (hasQuery || searchMode || current.isAChildOf (root));
    clearSearchButton.setVisible (hasQuery);

    const auto query = searchBox.getText().trim();
    for (size_t i = 0; i < chips.size(); ++i)
        chips[i].setToggleState (query.equalsIgnoreCase (kChipNames[i]), juce::dontSendNotification);

    const bool fileSelected = getSelectedFile() != nullptr;
    loadButton.setEnabled (fileSelected);
    playButton.setEnabled (fileSelected || wasPreviewing);
}

//==============================================================================
void SampleBrowser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto* item = getItem (row);
    if (item == nullptr)
        return;

    auto area = juce::Rectangle<int> (width, height);
    if (selected)
    {
        g.setColour (colours::body.withAlpha (0.13f));
        g.fillRect (area);
        g.setColour (colours::body);
        g.fillRect (area.withWidth (2));
    }

    area.removeFromLeft (9);
    area.removeFromRight (6);

    // Two lines for search results: the name, and the folder it's in.
    auto nameLine = searchMode ? area.removeFromTop (height / 2 + 3).withTrimmedTop (3) : area;
    const auto icon = nameLine.removeFromLeft (13).toFloat().withSizeKeepingCentre (13.0f, 11.0f);
    nameLine.removeFromLeft (8);

    const bool playing = wasPreviewing && ! item->isFolder && item->file == previewing;
    if (item->isFolder)
        drawFolderGlyph (g, icon, selected ? colours::text : colours::textDim);
    else if (playing)
        drawPlayGlyph (g, icon, colours::body);
    else
        drawWaveGlyph (g, icon, selected ? colours::body : colours::textDim);

    g.setFont (uiFont (12.5f, selected));
    g.setColour (playing ? colours::body : colours::text);
    g.drawText (item->name, nameLine, juce::Justification::centredLeft, true);

    if (searchMode)
    {
        g.setFont (uiFont (10.5f));
        g.setColour (colours::textDim);
        const auto detailArea = area.withTrimmedLeft (21).withTrimmedBottom (4);
        const auto parts = juce::StringArray::fromTokens (item->detail, juce::File::getSeparatorString(), {});
        g.drawText (fitPath (parts, g.getCurrentFont(), (float) detailArea.getWidth()), detailArea,
                    juce::Justification::centredLeft, true);
    }
}

void SampleBrowser::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    const auto* item = getItem (row);
    if (item != nullptr && ! item->isFolder && ! e.mods.isPopupMenu() && autoButton.getToggleState())
        preview (row);
}

void SampleBrowser::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    activate (row);
}

void SampleBrowser::selectedRowsChanged (int lastRowSelected)
{
    updateControls();

    // Clicks preview in listBoxItemClicked (which also covers clicking the row
    // that's already selected); this is for the arrow keys.
    if (suppressPreview || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;
    if (autoButton.getToggleState())
        preview (lastRowSelected);
}

juce::var SampleBrowser::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (rows.size() != 1)
        return {};

    const auto* item = getItem (rows[0]);
    return item != nullptr && ! item->isFolder ? juce::var (item->file.getFullPathName()) : juce::var();
}

juce::String SampleBrowser::getTooltipForRow (int row)
{
    const auto* item = getItem (row);
    return item != nullptr ? item->file.getFullPathName() : juce::String();
}

bool SampleBrowser::shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails& details,
                                                          juce::StringArray& files, bool& canMoveFiles)
{
    const auto path = details.description.toString();
    if (! juce::File::isAbsolutePath (path) || ! juce::File (path).existsAsFile())
        return false;

    files.add (path);
    canMoveFiles = false;
    return true;
}

//==============================================================================
bool SampleBrowser::keyPressed (const juce::KeyPress& key)
{
    // Keys the list and the search box leave alone.
    if (key == juce::KeyPress::leftKey)
    {
        goUp();
        return true;
    }
    if (key == juce::KeyPress::rightKey)
    {
        if (const auto* item = getItem (list.getSelectedRow()); item != nullptr && item->isFolder)
            navigateTo (item->file);
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        playButton.triggerClick();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && searchBox.getText().isNotEmpty())
    {
        setSearchText ({});
        return true;
    }
    return false;
}

void SampleBrowser::visibilityChanged()
{
    if (! isVisible() && previewing != juce::File() && processor.isPreviewing())
        processor.stopPreview();
}

//==============================================================================
juce::String SampleBrowser::breadcrumb (float width) const
{
    juce::StringArray parts;
    parts.add (root.getFileName());
    if (current != root && current.isAChildOf (root))
        parts.addTokens (current.getRelativePathFrom (root), juce::File::getSeparatorString(), {});
    parts.removeEmptyStrings();
    return fitPath (parts, uiFont (11.5f), width);
}

void SampleBrowser::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto title = titleArea;
    g.setColour (colours::body);
    g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) title.getX() + 3.0f,
                                                                     (float) title.getCentreY() + 1.0f }));
    title.removeFromLeft (12);
    g.setFont (uiFont (12.0f, true));
    g.setColour (colours::text);
    g.drawText ("SAMPLES", title.translated (0, 1), juce::Justification::centredLeft);

    g.setFont (uiFont (11.5f));
    g.setColour (searchMode ? colours::textDim.withAlpha (0.6f) : colours::textDim);
    const auto crumbs = searchMode ? "Searching all of " + root.getFileName() : breadcrumb ((float) pathArea.getWidth());
    g.drawText (crumbs, pathArea, juce::Justification::centredLeft, true);

    g.setFont (uiFont (11.0f));
    g.setColour (colours::textDim);
    g.drawText (status, statusArea, juce::Justification::centredRight, true);

    g.setFont (uiFont (11.0f, true));
    g.drawText ("LOAD INTO", targetLabelArea, juce::Justification::centredLeft, false);
}

void SampleBrowser::resized()
{
    constexpr int gap = 6;
    auto area = getLocalBounds().reduced (10, 0);

    titleArea = area.removeFromTop (28).withTrimmedLeft (2);

    placeBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (gap);

    auto nav = area.removeFromTop (22);
    upButton.setBounds (nav.removeFromLeft (26));
    nav.removeFromLeft (8);
    pathArea = nav;
    area.removeFromTop (gap);

    const auto searchRow = area.removeFromTop (26);
    searchBox.setBounds (searchRow);
    clearSearchButton.setBounds (searchRow.withLeft (searchRow.getRight() - 24).reduced (3));
    area.removeFromTop (gap);

    auto chipRow = area.removeFromTop (20);
    const int chipW = (chipRow.getWidth() - gap * ((int) chips.size() - 1)) / (int) chips.size();
    for (auto& chip : chips)
    {
        chip.setBounds (&chip == &chips.back() ? chipRow : chipRow.removeFromLeft (chipW));
        chipRow.removeFromLeft (gap);
    }
    area.removeFromTop (8);

    // Bottom up: target row, then the preview / status row, then the list gets the rest.
    area.removeFromBottom (10);
    auto targetRow = area.removeFromBottom (26);
    targetLabelArea = targetRow.removeFromLeft (66);
    loadButton.setBounds (targetRow.removeFromRight (56));
    targetRow.removeFromRight (gap);
    targetBox.setBounds (targetRow);
    area.removeFromBottom (gap);

    auto statusRow = area.removeFromBottom (22);
    autoButton.setBounds (statusRow.removeFromLeft (50));
    statusRow.removeFromLeft (gap);
    playButton.setBounds (statusRow.removeFromLeft (50));
    statusRow.removeFromLeft (gap);
    statusArea = statusRow;
    area.removeFromBottom (gap);

    list.setBounds (area);
}

} // namespace drum::ui
