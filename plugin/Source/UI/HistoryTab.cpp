#include "HistoryTab.h"
#include "Theme.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"
#include "Storage/CacheManager.h"

namespace context::ui
{
class HistoryTab::Row : public juce::Component
{
public:
    explicit Row (HistoryTab& o) : owner (o)
    {
        for (auto* b : { &star, &play, &use, &folder, &remove })
        {
            styleQuiet (*b);
            addAndMakeVisible (*b);
        }
        star.onClick   = [this] { owner.processor.services().history().setFavorite (item.generationId, ! item.favorite); };
        play.onClick   = [this] { owner.processor.previewResult (item); };
        use.onClick    = [this] { owner.processor.selectResult (item.generationId); if (owner.onUseResult) owner.onUseResult(); };
        folder.onClick = [this] { owner.processor.services().history().setPinned (item.generationId); item.localFile.revealToUser(); };
        remove.onClick = [this] { owner.removeEntry (item); };
        play.setTooltip ("Preview");
        use.setTooltip ("Open in the Generate screen (regenerate / variation / drag)");
        folder.setTooltip ("Show the file in Explorer");
        remove.setTooltip ("Remove from history");
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void set (const GenerationResult& r)
    {
        item = r;
        star.setButtonText (r.favorite ? glyph ("\xe2\x98\x85") : glyph ("\xe2\x98\x86"));
        star.setColour (juce::TextButton::textColourOffId, r.favorite ? colours::favorite : colours::dim);
        star.setTooltip (r.favorite ? "Unfavorite" : "Favorite (favorites are never removed by cache cleanup)");
        const bool exists = r.localFile.existsAsFile();
        play.setEnabled (exists);
        folder.setEnabled (exists);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (2, 2).toFloat();
        g.setColour (isMouseOver (true) ? colours::raised : colours::panel);
        g.fillRoundedRectangle (r, 6.0f);

        auto text = getLocalBounds().reduced (12, 6);
        text.removeFromLeft (40);
        text.removeFromRight (4 * 74 + 10);
        g.setColour (item.localFile.existsAsFile() ? colours::text : colours::faint);
        g.setFont (ContextLookAndFeel::font (14.0f, true));
        g.drawText (item.prompt.isNotEmpty() ? item.prompt : item.localFile.getFileName(), text.removeFromTop (20), juce::Justification::centredLeft, true);
        g.setColour (colours::dim);
        g.setFont (ContextLookAndFeel::font (12.0f));
        juce::String meta = toLabel (item.type) + "  " + glyph ("\xc2\xb7") + "  " + juce::String (item.durationSeconds, 1) + " s  "
                          + glyph ("\xc2\xb7") + "  " + item.creationTimestamp.formatted ("%d %b %Y %H:%M");
        if (item.bpm) meta << "  " << glyph ("\xc2\xb7") << "  " << juce::roundToInt (*item.bpm) << " BPM";
        if (item.key.isNotEmpty()) meta << " " << item.key << " " << item.mode;
        if (! item.localFile.existsAsFile()) meta << "  " << glyph ("\xc2\xb7") << "  file missing";
        else if (item.pinned) meta << "  " << glyph ("\xc2\xb7") << "  kept (used in a project)";
        g.drawText (meta, text, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 12);
        star.setBounds (r.removeFromLeft (32));
        for (auto* b : { &remove, &folder, &use, &play })
        {
            b->setBounds (r.removeFromRight (70));
            r.removeFromRight (4);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6 || ! item.localFile.existsAsFile())
            return;
        dragging = true;
        owner.processor.services().history().setPinned (item.generationId);
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ item.localFile.getFullPathName() }, false, this);
    }
    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    HistoryTab& owner;
    GenerationResult item;
    bool dragging = false;
    juce::TextButton star, play { glyph ("\xe2\x96\xb6") }, use { "Open" }, folder { "Folder" }, remove { "Remove" };
};

HistoryTab::HistoryTab (ContextAudioProcessor& p) : processor (p)
{
    for (auto* b : { &recentButton, &favoritesButton, &allButton })
    {
        styleQuiet (*b);
        addAndMakeVisible (*b);
    }
    recentButton.onClick    = [this] { filter = Filter::Recent; refresh(); };
    favoritesButton.onClick = [this] { filter = Filter::Favorites; refresh(); };
    allButton.onClick       = [this] { filter = Filter::All; refresh(); };

    list.setModel (this);
    list.setRowHeight (60);
    list.setColour (juce::ListBox::backgroundColourId, colours::background);
    list.setColour (juce::ListBox::outlineColourId, colours::background);
    addAndMakeVisible (list);

    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setColour (juce::Label::textColourId, colours::dim);
    addAndMakeVisible (emptyLabel);
    infoLabel.setColour (juce::Label::textColourId, colours::faint);
    infoLabel.setFont (ContextLookAndFeel::font (12.0f));
    infoLabel.setText ("Drag any row into your DAW. Favorites and sounds you dragged or exported are never removed by cache cleanup.",
                       juce::dontSendNotification);
    addAndMakeVisible (infoLabel);
    refresh();
}

HistoryTab::~HistoryTab()
{
    list.setModel (nullptr);
}

void HistoryTab::refresh()
{
    auto& h = processor.services().history();
    items = filter == Filter::Recent ? h.recent (25) : filter == Filter::Favorites ? h.favorites() : h.all();
    recentButton.setToggleState (filter == Filter::Recent, juce::dontSendNotification);
    favoritesButton.setToggleState (filter == Filter::Favorites, juce::dontSendNotification);
    allButton.setToggleState (filter == Filter::All, juce::dontSendNotification);
    emptyLabel.setText (filter == Filter::Favorites ? "No favorites yet. Star a sound to keep it here."
                                                    : "Nothing generated yet. Your sounds will show up here.",
                        juce::dontSendNotification);
    emptyLabel.setVisible (items.empty());
    list.updateContent();
    list.repaint();
}

int HistoryTab::getNumRows() { return (int) items.size(); }

juce::Component* HistoryTab::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    if (row < 0 || row >= (int) items.size())
    {
        delete existing;
        return nullptr;
    }
    auto* r = dynamic_cast<Row*> (existing);
    if (r == nullptr)
    {
        delete existing;
        r = new Row (*this);
    }
    r->set (items[(size_t) row]);
    return r;
}

void HistoryTab::removeEntry (const GenerationResult& item)
{
    const auto folder = processor.services().generatedFolder();
    const bool deletable = CacheManager::isDeletable (item, folder, build::productName);
    juce::Component::SafePointer<HistoryTab> safe (this);
    const auto id = item.generationId;
    const auto file = item.localFile;

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Remove from history?")
                                      .withMessage (deletable ? "The audio file will also be deleted from Context's cache folder."
                                                              : "The audio file itself will be kept (it is a favorite, was used in a project, or lives outside Context's cache).")
                                      .withButton ("Remove")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [safe, id, file, deletable] (int result)
    {
        // Only act if the editor (and therefore the processor and its services) still exists.
        if (result != 1 || safe == nullptr)
            return;
        safe->processor.services().history().remove (id);
        if (deletable)
            file.deleteFile();
        safe->refresh();
    });
}

void HistoryTab::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

void HistoryTab::resized()
{
    auto r = getLocalBounds().reduced (14, 12);
    auto top = r.removeFromTop (32);
    for (auto* b : { &recentButton, &favoritesButton, &allButton })
    {
        b->setBounds (top.removeFromLeft (110));
        top.removeFromLeft (6);
    }
    r.removeFromTop (8);
    infoLabel.setBounds (r.removeFromBottom (20));
    list.setBounds (r);
    emptyLabel.setBounds (r);
}
} // namespace context::ui
