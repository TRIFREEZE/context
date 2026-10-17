#pragma once

#include "Theme.h"
#include "Motion.h"

namespace context::ui
{
class Sheet;

/** Implemented by the editor: presents one sheet at a time over the whole plugin window. */
class SheetHost
{
public:
    virtual ~SheetHost() = default;
    virtual void presentSheet (std::unique_ptr<Sheet>) = 0;
    virtual void dismissSheet (Sheet*) = 0;
};

/** Context's own dialog: a panel over a dimmed plugin, in the plugin's look, instead of an
    OS message box. Every confirmation, warning and "details" view uses it, so all of them
    look, animate and behave the same (Esc / clicking outside = the cancel action).

        Sheet::show (*this, "Remove from history?", "The audio file will also be deleted.",
                     { { "Remove", ButtonStyle::Danger, [this] { ... } },
                       { "Cancel" } });                                                        */
class Sheet : public juce::Component
{
public:
    struct Action
    {
        juce::String text;
        ButtonStyle style = ButtonStyle::Secondary;
        std::function<void()> run;
    };

    /** Shows over the editor that contains `from`. Returns false (nothing shown) when `from`
        is not inside an editor (tests, or after the editor closed). */
    static bool show (juce::Component& from, const juce::String& title, const juce::String& body,
                      std::vector<Action> actions, std::unique_ptr<juce::Component> extra = {}, int extraHeight = 0);

    Sheet (const juce::String& title, const juce::String& body, std::vector<Action>, std::unique_ptr<juce::Component> extra, int extraHeight);

    void appear();
    void close (int actionIndex);    // runs the action after the sheet has gone (-1 = cancel)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    SheetHost* host = nullptr;

private:
    juce::Rectangle<int> panelBounds() const;
    int bodyHeight (int width) const;

    juce::String title, body;
    std::vector<Action> actions;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::Component> extra;
    int extraHeight = 0;
    motion::Value shown { 0.0f };
    bool closing = false;
};
} // namespace context::ui
