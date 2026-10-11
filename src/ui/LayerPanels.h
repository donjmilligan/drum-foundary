// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "Controls.h"
#include "SampleSlotWidget.h"

namespace drum::ui
{

/** Titled panel with its controls in a grid (row-major, `columns` wide), painted like
    Section. The title bar can carry a component before the title (a mute button) and
    others at its right end (a style selector), and a full-width strip (a sample slot)
    can sit between the title bar and the grid, with components to its left (selectors).
    Rows share the height left over.

    A frameless GridPanel draws nothing and has no title bar: it's a page inside
    another panel (see LayersSection). */
class GridPanel : public juce::Component
{
public:
    GridPanel (const juce::String& title, juce::Colour accent, int columns);

    void setTitle (const juce::String& newTitle);
    const juce::String& getTitle() const noexcept { return title; }
    /** No background, border or title bar (for a page inside another panel). */
    void setFrameless (bool shouldBeFrameless);
    /** Puts `c` before the title, `width` wide (nullptr for none). It must already be a child. */
    void setTitleLeft (juce::Component* c, int width, int height = 18);
    /** Adds `c` at the right end of the title bar, left of the ones added before.
        Hidden ones take no room. */
    void addTitleRight (juce::Component& c, int width, int height = 22);
    /** A row under the title bar, `height` tall, filled by `c` (it can be nullptr, for a
        row of strip-left components only; a height of 0 removes the row). `c` must
        already be a child. */
    void setStrip (juce::Component* c, int height);
    /** Adds `c` at the left of the strip row, right of the ones added before. Hidden
        ones take no room. */
    void addStripLeft (juce::Component& c, int width, int height = 26);
    /** Next grid cell. With `pillHeight` > 0 the component is a pill of that height,
        centred where a knob in the same row would be. */
    void addCell (juce::Component& c, int pillHeight = 0);
    /** Forgets the grid cells (they stay children). */
    void clearCells();
    /** Outlines the panel in its accent (e.g. while it's the browser's target). */
    void setHighlighted (bool shouldBeHighlighted);

    juce::Colour getAccent() const noexcept { return accent; }

    void paint (juce::Graphics&) override;
    void resized() override;

protected:
    /** The part of the title bar between the title text and the right-hand components. */
    juce::Rectangle<int> getTitleFreeArea() const noexcept { return titleFree; }
    /** Where the strip and grid go. */
    juce::Rectangle<int> getContentArea() const noexcept { return content; }
    /** The strip row right of its strip-left components (where the strip itself goes). */
    juce::Rectangle<int> getStripArea() const noexcept { return stripArea; }

private:
    struct Cell
    {
        juce::Component* component = nullptr;
        int pillHeight = 0;
    };
    struct Item
    {
        juce::Component* component = nullptr;
        int width = 0, height = 0;
    };

    juce::String title;
    juce::Colour accent;
    int columns;
    bool highlighted = false, frameless = false;

    juce::Component* titleLeft = nullptr;
    int titleLeftWidth = 0, titleLeftHeight = 18;
    std::vector<Item> titleRight, stripLeft;
    juce::Component* strip = nullptr;
    int stripHeight = 0;
    std::vector<Cell> cells;

    juce::Rectangle<int> titleText, titleFree, content, stripArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GridPanel)
};

//==============================================================================
/** One of the core layers (Body, Noise, Click): its style selector, its sample slot,
    and its knobs. Knobs whose job depends on the style are relabelled from
    StyleInfo.h, and dimmed where the style doesn't use them (they stay editable).
    The whole layer dims while it's muted.

    As a panel (Body) it has a title bar with mute, solo and polarity (Ø) buttons, the
    name and the style selector. As a page (Noise and Click, inside LayersSection) it
    has no frame: Ø and the style selector sit left of the sample slot, and the tab
    carries M and S. */
class LayerPanel : public GridPanel
{
public:
    /** A knob's caption in a style, or nullptr when that style doesn't use it. */
    using CaptionFn = const char* (*) (int style);
    using UsesSampleFn = bool (*) (int style);

    LayerPanel (DrumFoundryProcessor& processor, const juce::String& title, juce::Colour accent,
                drum::SampleSlot slot, const char* onId, const char* soloId, const char* styleId,
                const char* invertId, UsesSampleFn usesSample, int columns, bool asPage = false);

    /** Adds a knob to the grid. With `captionFor`, it's relabelled / dimmed per style. */
    void addKnob (Knob& knob, CaptionFn captionFor = nullptr);
    /** Adds a knob that only does something when the style plays the sample. */
    void addSampleKnob (Knob& knob);

    SampleSlotWidget& getSlotWidget() noexcept { return slotWidget; }
    drum::SampleSlot getSlot() const noexcept { return slotWidget.getSlot(); }
    const juce::String& getOnParamId() const noexcept { return onParamId; }
    const juce::String& getSoloParamId() const noexcept { return soloParamId; }
    /** The title bar's mute button (nullptr for a page: its tab has one). */
    MuteButton* getMuteButton() noexcept { return switches != nullptr ? &switches->mute : nullptr; }
    /** The title bar's solo button (nullptr for a page: its tab has one). */
    SoloButton* getSoloButton() noexcept { return switches != nullptr ? &switches->solo : nullptr; }
    /** The polarity (Ø) button: on the title bar, or left of a page's style selector. */
    InvertButton& getInvertButton() noexcept { return switches != nullptr ? *switches->invert : *pageInvert; }
    /** True while another layer is soloed and this one isn't. */
    bool isImplicitlyMuted() const noexcept { return shownSilent == 1; }

    /** Re-reads On, Solo and the style, and updates the knobs if any changed. The editor
        calls this from its timer (parameters can change on any thread). */
    void update();

    void paint (juce::Graphics&) override;

private:
    struct Entry
    {
        Knob* knob = nullptr;
        juce::String caption, paramName;
        CaptionFn captionFor = nullptr;
        bool sampleOnly = false;
    };

    void apply (bool on, int style);

    const juce::String onParamId, soloParamId;
    std::atomic<float>* onValue = nullptr;
    std::atomic<float>* soloValue = nullptr;
    SoloWatcher soloWatcher;
    std::atomic<float>* styleValue = nullptr;
    juce::StringArray styleNames;
    UsesSampleFn usesSample;

    std::unique_ptr<LayerSwitches> switches;
    std::unique_ptr<InvertButton> pageInvert; // a page's Ø (a panel's is in `switches`)
    ChoiceSelector styleBox;
    SampleSlotWidget slotWidget;
    std::vector<Entry> entries;
    int shownOn = -1, shownStyle = -1, shownSilent = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerPanel)
};

//==============================================================================
/** Every layer except Body, as tabs: NOISE and CLICK (the core layers), then one tab
    per extra layer, named by what it is ("BODY 2", "NOISE 2", "CLICK 2", "SAMPLE",
    "SINE"), then "+", which adds a Body, Noise, Click, Sample or Sine layer. Each
    page has a polarity (Ø) button left of its selectors. Each tab has mute (M)
    and solo (S) buttons; clicking them (or the page's Ø) also switches to that tab and
    brings that layer's curves to the display (out of the LFO tab if it's on show). An extra layer's tab also
    has ×, which removes the layer (switches it off and closes the tab). While any
    layer is soloed, the others read as silenced (dimmed, with an outlined M). The
    selected tab's page fills the rest of the panel.

    An extra layer has a tab while it's on, and keeps it when it's muted (switched off
    from here or anywhere else). Loading a preset or a session forgets muted layers:
    after that, the layers that are on have tabs. */
class LayersSection : public juce::Component
{
public:
    /** Tab indices: the core layers, then extra layer k at firstExtraTab + k. */
    enum { noiseTab = 0, clickTab = 1, firstExtraTab = 2, numTabs = firstExtraTab + kNumExtraLayers };

    /** `noisePage` and `clickPage` are the core layers' pages (made with asPage). */
    LayersSection (DrumFoundryProcessor& processor, LayerPanel& noisePage, LayerPanel& clickPage);
    ~LayersSection() override;

    /** BROWSE on a sample slot, and adding a Sample layer. */
    std::function<void (drum::SampleSlot)> onBrowse;
    /** The user clicked a tab (its name, M or S, not ×) or pressed anywhere on a page:
        the tab it belongs to, and whether it was the tab itself (`fromTab`). The display
        uses it to edit that layer's curves. */
    std::function<void (int tab, bool fromTab)> onLayerTouched;
    /** What clicking a tab, or its M or S, does: selects it and reports it to onLayerTouched. */
    void touchTab (int tab);

    /** Follows the parameters (the editor calls this from its timer). */
    void update();
    /** After a preset or session load: forgets muted extra layers and selects the first tab. */
    void resetSelection();
    /** After an undo or redo: takes the extra-layer tabs the undo history put back in
        the processor's UiState, and keeps the selected tab if it still has one. */
    void restoreTabs();

    /** Shows a tab. An extra layer that has no tab gets one while it's selected. */
    void selectTab (int tab);
    int getSelectedTab() const noexcept { return selected; }
    /** Shows extra layer `index` (0-based), whether it's on or not. */
    void selectExtraLayer (int index) { selectTab (firstExtraTab + index); }
    /** The selected extra layer (0-based), or -1 when a core layer is shown. */
    int getSelectedExtraLayer() const noexcept { return selected >= firstExtraTab ? selected - firstExtraTab : -1; }
    /** Shows the tab that owns a sample slot (Noise, Click or an extra layer). */
    void showSlot (drum::SampleSlot slot);

    /** Adds an extra layer playing `source` (an index into layerSourceNames) in the first
        free slot, with fresh settings, and selects it. A Sample layer also opens the
        browser on it (onBrowse). Returns the layer's index, or -1 when all are in use. */
    int addLayer (int source);
    /** Switches extra layer `index` off and closes its tab. */
    void removeLayer (int index);
    /** True if extra layer `index` has a tab. */
    bool hasTab (int index) const noexcept;
    /** The tab's label, e.g. "NOISE 2" (empty for an extra layer without a tab). */
    juce::String getTabName (int tab) const;
    /** The mute button on a tab. */
    MuteButton& getMuteButton (int tab);
    /** The solo button on a tab. */
    SoloButton& getSoloButton (int tab);
    /** The polarity (Ø) button on a tab's page. */
    InvertButton& getInvertButton (int tab);

    /** Outlines the panel (while the browser loads into one of its layers). */
    void setHighlighted (bool shouldBeHighlighted);

    void paint (juce::Graphics&) override;
    void resized() override;
    /** Presses on the pages (it listens to them). */
    void mouseDown (const juce::MouseEvent&) override;

private:
    class Tab;
    class GlyphButton;
    struct ExtraPage;

    void refresh();
    void showAddMenu();
    int findFreeLayer() const noexcept;
    bool isPresent (int index) const noexcept;
    void storeUiState();

    DrumFoundryProcessor& processor;
    LayerPanel& noisePage;
    LayerPanel& clickPage;
    struct LayerValues
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* source = nullptr;
        std::atomic<float>* noiseStyle = nullptr;
        std::atomic<float>* bodyStyle = nullptr;
        std::atomic<float>* clickType = nullptr;
        std::atomic<float>* solo = nullptr;
    };

    std::array<LayerValues, kNumExtraLayers> values;
    std::array<std::unique_ptr<ExtraPage>, kNumExtraLayers> extraPages;
    std::array<std::unique_ptr<Tab>, numTabs> tabs;
    juce::TextButton addButton { "+" };

    int selected = noiseTab;
    std::array<bool, kNumExtraLayers> on {}, kept {};
    std::array<int, kNumExtraLayers> sources {}, styles {};
    std::array<bool, kNumExtraLayers> silent {};
    SoloWatcher soloWatcher;
    bool highlighted = false, firstUpdate = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayersSection)
};

} // namespace drum::ui
