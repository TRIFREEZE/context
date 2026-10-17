#include "HistoryTab.h"
#include "Theme.h"
#include "Sheet.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"
#include "Storage/CacheManager.h"

namespace context::ui
{
namespace
{
    // Column layout shared by the header and the rows (x offsets from the left edge).
    struct Columns
    {
        juce::Rectangle<int> star, name, type, length, musical, made, actions;
        explicit Columns (juce::Rectangle<int> r)
        {
            star = r.removeFromLeft (30);
            actions = r.removeFromRight (236);
            made = r.removeFromRight (110);
            musical = r.removeFromRight (130);
            length = r.removeFromRight (64);
            type = r.removeFromRight (92);
            name = r.withTrimmedRight (12);
        }
    };
}

class HistoryTab::Row : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Row (HistoryTab& o) : owner (o)
    {
        for (auto* b : { (juce::Button*) &star, (juce::Button*) &play, (juce::Button*) &folder, (juce::Button*) &remove })
            addAndMakeVisible (b);
        setStyle (use, ButtonStyle::Secondary);
        addAndMakeVisible (use);
        star.setOnColour (colours::favorite);
        star.onClick   = [this] { owner.processor.services().history().setFavorite (item.generationId, ! item.favorite); };
        play.onClick   = [this] { owner.processor.previewResult (item); };
        use.onClick    = [this] { owner.processor.selectResult (item.generationId); if (owner.onUseResult) owner.onUseResult(); };
        folder.onClick = [this] { owner.processor.services().history().setPinned (item.generationId); item.localFile.revealToUser(); };
        remove.onClick = [this] { owner.removeEntry (item); };
        use.setTooltip ("Load into the Sound lane (fit, edit, process, drag)");
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag into your DAW");
    }

    void set (const GenerationResult& r)
    {
        item = r;
        star.setIcon (r.favorite ? IconButton::Icon::StarFilled : IconButton::Icon::Star);
        star.setToggleState (r.favorite, juce::dontSendNotification);
        star.setTooltip (r.favorite ? "Unfavorite" : "Favorite (favorites are never removed by cache cleanup)");
        const bool exists = r.localFile.existsAsFile();
        play.setEnabled (exists);
        folder.setEnabled (exists);
        use.setEnabled (exists);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float hv = motion::hoverAmount (*this, isMouseOver (true));
        if (hv > 0.001f)
        {
            g.setColour (colours::panel.withAlpha (hv));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (4.0f, 2.0f), radius::panel);
        }
        g.setColour (colours::line);
        g.fillRect (12, getHeight() - 1, getWidth() - 24, 1);

        Columns c (getLocalBounds().reduced (12, 0));
        const bool exists = item.localFile.existsAsFile();
        g.setColour (exists ? colours::text : colours::faint);
        g.setFont (ui::font (type::body + 0.5f, Weight::Medium));
        auto nameArea = c.name;
        g.drawText (fitText (item.prompt.isNotEmpty() ? item.prompt : item.localFile.getFileName(), ui::font (type::body + 0.5f, Weight::Medium),
                             (float) nameArea.getWidth()),
                    nameArea.removeFromTop (getHeight() / 2 + 4).withTrimmedTop (6), juce::Justification::bottomLeft, false);
        juce::String note;
        if (! exists) note = "File missing";
        else if (item.pinned) note = "Used in a project, kept";
        else if (item.metadata.contains ("contextMode")) note = "Made on your track";
        if (note.isNotEmpty())
        {
            g.setColour (exists ? colours::faint : colours::warn);
            g.setFont (ui::font (type::meta));
            g.drawText (note, nameArea.withTrimmedTop (2), juce::Justification::topLeft, true);
        }

        g.setColour (colours::dim);
        g.setFont (ui::font (type::label));
        g.drawText (toLabel (item.type), c.type, juce::Justification::centredLeft, true);
        g.setFont (numberFont (type::label, Weight::Regular));
        g.drawText (juce::String (item.durationSeconds, 1) + " s", c.length, juce::Justification::centredLeft, true);
        juce::String musical;
        if (item.bpm) musical << juce::roundToInt (*item.bpm) << " BPM";
        if (item.key.isNotEmpty()) musical << (musical.isNotEmpty() ? "  " : "") << item.key << (item.mode == "minor" ? "m" : "");
        g.drawText (musical, c.musical, juce::Justification::centredLeft, true);
        g.setColour (colours::faint);
        g.drawText (item.creationTimestamp.formatted ("%d %b, %H:%M"), c.made, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        Columns c (getLocalBounds().reduced (12, 0));
        star.setBounds (c.star.withSizeKeepingCentre (26, 26));
        auto a = c.actions.withSizeKeepingCentre (c.actions.getWidth(), height::control + 2);
        remove.setBounds (a.removeFromRight (30));
        a.removeFromRight (2);
        folder.setBounds (a.removeFromRight (30));
        a.removeFromRight (6);
        use.setBounds (a.removeFromRight (70));
        a.removeFromRight (6);
        play.setBounds (a.removeFromRight (30));
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
    IconButton star { IconButton::Icon::Star, "Favorite" }, play { IconButton::Icon::Play, "Preview" },
               folder { IconButton::Icon::Folder, "Show the file in Explorer" }, remove { IconButton::Icon::Trash, "Remove from history" };
    juce::TextButton use { "Load" };
};

HistoryTab::HistoryTab (ContextAudioProcessor& p) : processor (p)
{
    filterSwitch.setItems ({ "Recent", "Favorites", "Everything" }, { "The last 25 sounds", "Starred sounds", "Every sound in your library" });
    filterSwitch.onChange = [this] (int i) { filter = i == 1 ? Filter::Favorites : i == 2 ? Filter::All : Filter::Recent; refresh(); };
    addAndMakeVisible (filterSwitch);

    list.setModel (this);
    list.setRowHeight (54);
    list.setColour (juce::ListBox::backgroundColourId, colours::background);
    list.setColour (juce::ListBox::outlineColourId, colours::background);
    addAndMakeVisible (list);

    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setColour (juce::Label::textColourId, colours::dim);
    addAndMakeVisible (emptyLabel);
    infoLabel.setColour (juce::Label::textColourId, colours::faint);
    infoLabel.setFont (ui::font (type::label));
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
    filterSwitch.setSelected (filter == Filter::Favorites ? 1 : filter == Filter::All ? 2 : 0);
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

    Sheet::show (*this, "Remove from history?",
                 deletable ? "The audio file will also be deleted from Context's cache folder."
                           : "The audio file itself is kept (it is a favorite, was used in a project, or lives outside Context's cache).",
                 { { "Cancel", ButtonStyle::Secondary, {} },
                   { "Remove", ButtonStyle::Danger, [safe, id, file, deletable]
                     {
                         // Only act if the editor (and therefore the processor and its services) still exists.
                         if (safe == nullptr)
                             return;
                         safe->processor.services().history().remove (id);
                         if (deletable)
                             file.deleteFile();
                         safe->refresh();
                     } } });
}

void HistoryTab::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    if (items.empty())
        return;
    Columns c (columnHeader.reduced (12, 0));
    g.setColour (colours::faint);
    g.setFont (labelFont());
    g.drawText ("Sound", c.name, juce::Justification::centredLeft);
    g.drawText ("Type", c.type, juce::Justification::centredLeft);
    g.drawText ("Length", c.length, juce::Justification::centredLeft);
    g.drawText ("Tempo and key", c.musical, juce::Justification::centredLeft);
    g.drawText ("Made", c.made, juce::Justification::centredLeft);
}

void HistoryTab::resized()
{
    auto r = getLocalBounds().reduced (space::l, space::m);
    auto top = r.removeFromTop (height::control + 2);
    filterSwitch.setBounds (top.removeFromLeft (260));
    r.removeFromTop (space::m);
    infoLabel.setBounds (r.removeFromBottom (20));
    columnHeader = r.removeFromTop (22);
    list.setBounds (r);
    emptyLabel.setBounds (r);
}
} // namespace context::ui
