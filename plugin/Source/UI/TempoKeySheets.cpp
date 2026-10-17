#include "TempoKeySheets.h"
#include "Sheet.h"
#include "Controls.h"
#include "PluginProcessor.h"

namespace context::ui
{
namespace
{
    juce::String bpmText (double bpm)
    {
        return juce::String (bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }

    // ---------------------------------------------------------------- tempo
    struct TempoChoice { double bpm = 0.0; };

    class TempoEditor final : public juce::Component
    {
    public:
        TempoEditor (std::shared_ptr<TempoChoice> c, double current, std::optional<double> daw)
            : choice (std::move (c))
        {
            field.setJustification (juce::Justification::centredLeft);
            field.setFont (numberFont (22.0f, Weight::SemiBold));
            field.setInputRestrictions (6, "0123456789.");
            field.setIndents (10, 6);
            field.setText (bpmText (current), false);
            field.onTextChange = [this] { choice->bpm = field.getText().getDoubleValue(); };
            field.onReturnKey = [this]
            {
                if (auto* sheet = findParentComponentOfClass<Sheet>())
                    sheet->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            };
            addAndMakeVisible (field);
            choice->bpm = current;

            auto quick = [this] (juce::TextButton& b, const juce::String& text, double value)
            {
                b.setButtonText (text);
                setStyle (b, ButtonStyle::Secondary);
                b.onClick = [this, value]
                {
                    field.setText (bpmText (value), juce::sendNotification);
                    field.grabKeyboardFocus();
                    field.selectAll();
                };
                addAndMakeVisible (b);
            };
            const double base = current;
            if (musical::isValidBpm (base / 2.0)) quick (half, "Half-time  " + bpmText (base / 2.0), base / 2.0);
            if (musical::isValidBpm (base * 2.0)) quick (dbl, "Double-time  " + bpmText (base * 2.0), base * 2.0);
            if (daw && std::abs (*daw - base) > 0.005) quick (dawButton, "DAW  " + bpmText (*daw), *daw);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto top = r.removeFromTop (40);
            field.setBounds (top.removeFromLeft (120));
            top.removeFromLeft (8);
            auto unit = top.removeFromLeft (40);
            juce::ignoreUnused (unit);
            r.removeFromTop (10);
            for (auto* b : { &half, &dbl, &dawButton })
                if (b->isVisible())
                {
                    const int w = juce::GlyphArrangement::getStringWidthInt (font (type::body, Weight::Medium), b->getButtonText()) + 26;
                    b->setBounds (r.removeFromLeft (w).withHeight (height::control + 2));
                    r.removeFromLeft (6);
                }
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (colours::faint);
            g.setFont (font (type::label, Weight::Medium));
            g.drawText ("BPM", juce::Rectangle<int> (128, 0, 60, 40), juce::Justification::centredLeft);
        }

        void visibilityChanged() override
        {
            if (isShowing())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TempoEditor> (this)]
                {
                    if (safe != nullptr && safe->isShowing()) { safe->field.grabKeyboardFocus(); safe->field.selectAll(); }
                });
        }

    private:
        std::shared_ptr<TempoChoice> choice;
        juce::TextEditor field;
        juce::TextButton half, dbl, dawButton;
    };

    // ---------------------------------------------------------------- key
    struct KeyChoice { int root = 0; bool minor = true; };

    class KeyPicker final : public juce::Component
    {
    public:
        explicit KeyPicker (std::shared_ptr<KeyChoice> c) : choice (std::move (c))
        {
            juce::StringArray roots;
            for (int i = 0; i < 12; ++i) roots.add (pitchClassName (i));
            root.setItems (roots);
            root.setFontSize (type::body);
            root.setSelected (choice->root);
            root.onChange = [this] (int i) { choice->root = i; };
            mode.setItems ({ "Major", "Minor" });
            mode.setFontSize (type::body);
            mode.setSelected (choice->minor ? 1 : 0);
            mode.onChange = [this] (int i) { choice->minor = i == 1; };
            addAndMakeVisible (root);
            addAndMakeVisible (mode);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            root.setBounds (r.removeFromTop (height::large));
            r.removeFromTop (10);
            mode.setBounds (r.removeFromTop (height::large).withWidth (180));
        }

    private:
        std::shared_ptr<KeyChoice> choice;
        SegmentedControl root, mode;
    };
}

// -------------------------------------------------------------------- ValueChip

void ValueChip::setText (const juce::String& t, juce::Font f, juce::Colour c)
{
    if (t == text && c == colour && f == textFont)
        return;
    text = t;
    textFont = f;
    colour = c;
    repaint();
}

void ValueChip::setUnit (const juce::String& u)
{
    if (u != unit) { unit = u; repaint(); }
}

int ValueChip::unitWidth() const
{
    return unit.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt (font (type::meta, Weight::Medium), unit) + 5;
}

int ValueChip::idealWidth() const
{
    return juce::GlyphArrangement::getStringWidthInt (textFont, text) + unitWidth() + 24;
}

void ValueChip::paint (juce::Graphics& g)
{
    const bool over = isMouseOver (true) && isEnabled();
    const float hv = motion::hoverAmount (*this, over);
    auto r = getLocalBounds().toFloat();
    if (hv > 0.001f)
    {
        g.setColour (colours::raised.withAlpha (hv));
        g.fillRoundedRectangle (r, radius::control);
        g.setColour (colours::border.withAlpha (hv));
        g.drawRoundedRectangle (r.reduced (0.5f), radius::control, 1.0f);
    }
    g.setColour (colour);
    g.setFont (textFont);
    auto textArea = r.withTrimmedLeft (5.0f).withTrimmedRight (16.0f);
    if (unit.isNotEmpty())
    {
        const float tw = (float) juce::GlyphArrangement::getStringWidthInt (textFont, text);
        g.drawText (text, textArea.removeFromLeft (tw + 1.0f), juce::Justification::centredLeft, false);
        g.setColour (colours::faint);
        g.setFont (font (type::meta, Weight::Medium));
        g.drawText (unit, textArea.withTrimmedLeft (5.0f).translated (0.0f, 1.0f), juce::Justification::centredLeft, false);
    }
    else
        g.drawText (text, textArea, juce::Justification::centredLeft, true);
    // Chevron: hinted at rest, clear on hover.
    juce::Path chevron;
    const float cx = r.getRight() - 9.0f, cy = r.getCentreY();
    chevron.startNewSubPath (cx - 3.0f, cy - 1.5f);
    chevron.lineTo (cx, cy + 1.5f);
    chevron.lineTo (cx + 3.0f, cy - 1.5f);
    g.setColour (colours::faint.withAlpha (0.35f + 0.65f * hv));
    g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ValueChip::mouseUp (const juce::MouseEvent& e)
{
    if (isEnabled() && getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

// -------------------------------------------------------------------- sheets

void showTempoSheet (juce::Component& from, ContextAudioProcessor& processor)
{
    auto& s = processor.session();
    const auto st = s.getState();
    const double current = s.sessionTempo().bpm;
    const auto daw = s.hostBpm();
    auto choice = std::make_shared<TempoChoice>();

    juce::String body;
    if (st.context)
        body << "Your track was captured as " << st.context->bars << " bars at " << bpmText (st.context->tempo.bpm) << " BPM. "
             << "Half-time or double-time reads the same audio at that tempo (nothing is stretched). Any other tempo "
             << "re-fits your track to it.";
    else
        body << "Context uses this tempo for bars, fitting and generation"
             << (daw ? " instead of the DAW's (" + bpmText (*daw) + " BPM)." : juce::String ("."));

    std::vector<Sheet::Action> actions;
    if (! st.context && st.tempoOverride && daw)
        actions.push_back ({ "Use DAW tempo", ButtonStyle::Secondary, [&processor] { processor.session().clearTempo(); } });
    actions.push_back ({ "Cancel", ButtonStyle::Secondary, {} });
    actions.push_back ({ "Set tempo", ButtonStyle::Primary, [&processor, choice]
    {
        if (musical::isValidBpm (choice->bpm))
            processor.session().setTempo (choice->bpm);
    } });
    Sheet::show (from, "Tempo", body, std::move (actions), std::make_unique<TempoEditor> (choice, current, daw), 40 + 10 + height::control + 2);
}

void showKeySheet (juce::Component& from, ContextAudioProcessor& processor)
{
    auto& s = processor.session();
    const auto key = s.sessionKey();
    const auto detected = s.detectedKey();
    auto choice = std::make_shared<KeyChoice>();
    if (key.known())
    {
        choice->root = key.root;
        choice->minor = key.minor;
    }

    juce::String body;
    if (detected.known())
        body << "Detected: " << detected.display() << " (" << juce::roundToInt (detected.confidence * 100.0) << "% sure). ";
    else
        body << "Context hasn't heard a clear key yet. ";
    body << "Generations are asked to be in this key, unless your prompt names one.";

    std::vector<Sheet::Action> actions;
    actions.push_back ({ "Detect automatically", ButtonStyle::Secondary, [&processor] { processor.session().setKey (-1, false); } });
    actions.push_back ({ "Cancel", ButtonStyle::Secondary, {} });
    actions.push_back ({ "Use this key", ButtonStyle::Primary, [&processor, choice] { processor.session().setKey (choice->root, choice->minor); } });
    Sheet::show (from, "Key", body, std::move (actions), std::make_unique<KeyPicker> (choice), height::large * 2 + 10);
}
} // namespace context::ui
