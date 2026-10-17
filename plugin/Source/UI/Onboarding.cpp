#include "Onboarding.h"
#include "Theme.h"
#include "PluginProcessor.h"
#include "Utils/UrlSafety.h"

namespace context::ui
{
Onboarding::Onboarding (ContextAudioProcessor& p) : processor (p)
{
    for (auto* e : { &key, &address })
    {
        e->setFont (ui::font (16.0f));
        e->setIndents (10, 8);
        addAndMakeVisible (*e);
    }
    key.setTextToShowWhenEmpty ("CTX-XXXX-XXXX-XXXX-XXXX", colours::faint);
    key.setInputRestrictions (40);
    key.onReturnKey = [this] { activate.triggerClick(); };
    address.setText (processor.services().settings().get().bridgeUrl, juce::dontSendNotification);
    address.onReturnKey = [this] { test.triggerClick(); };

    stylePrimary (activate);
    stylePrimary (test);
    for (auto* b : { &skip, &useMock, &done })
        styleQuiet (*b);

    activate.onClick = [this]
    {
        processor.services().license().activate (key.getText());
        refresh();
    };
    skip.onClick = [this] { skippedLicense = true; refresh(); };
    test.onClick = [this]
    {
        const auto parsed = urls::parseBaseUrl (address.getText().trim());
        if (! parsed.ok)
        {
            message.setText ("That address isn't valid: " + parsed.error, juce::dontSendNotification);
            return;
        }
        processor.services().settings().update ([parsed] (Settings& s) { s.providerId = "bridge"; s.bridgeUrl = parsed.normalized; });
        processor.rebuildProvider();
        processor.generation().testConnection();
        refresh();
    };
    useMock.onClick = [this]
    {
        processor.services().settings().update ([] (Settings& s) { s.providerId = "mock"; });
        processor.rebuildProvider();
        processor.generation().testConnection();
        finish();
    };
    done.onClick = [this] { finish(); };

    message.setFont (ui::font (type::body + 1.0f));
    message.setJustificationType (juce::Justification::topLeft);
    message.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (message);

    for (auto* b : { &activate, &skip, &test, &useMock, &done })
        addAndMakeVisible (*b);
    refresh();
}

int Onboarding::step() const
{
    return (processor.isLicensed() || skippedLicense) ? 2 : 1;
}

void Onboarding::finish()
{
    processor.services().settings().update ([] (Settings& s) { s.firstRunComplete = true; });
    if (onFinished)
        onFinished();
}

void Onboarding::refresh()
{
    const int s = step();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &key, &activate, &skip })
        c->setVisible (s == 1);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &address, &test, &useMock, &done })
        c->setVisible (s == 2);

    if (s == 1)
    {
        auto& lic = processor.services().license();
        const auto st = lic.getStatus();
        activate.setEnabled (st != LicenseManager::Status::Activating && st != LicenseManager::Status::NotConfigured);
        message.setText (st == LicenseManager::Status::Activating ? "Activating..." : lic.getMessage(), juce::dontSendNotification);
        message.setColour (juce::Label::textColourId, st == LicenseManager::Status::Unlicensed || st == LicenseManager::Status::Activating
                                                          ? colours::dim : colours::warn);
    }
    else
    {
        const auto conn = processor.generation().getConnectionResult();
        juce::String text;
        juce::Colour c = colours::dim;
        switch (conn.status)
        {
            case ConnectionStatus::Connected:  text = glyph ("\xe2\x9c\x93  ") + "Local generation bridge detected."; c = colours::good; break;
            case ConnectionStatus::MockReady:  text = glyph ("\xe2\x9c\x93  ") + "Mock Provider ready (offline test sounds)."; c = colours::good; break;
            case ConnectionStatus::Testing:    text = "Testing..."; break;
            case ConnectionStatus::Unknown:    text = "Start your local generation bridge, then click Test Connection.\n"
                                                      "Context never asks for your provider password, cookies or tokens: the bridge handles sign-in itself."; break;
            case ConnectionStatus::ConnectionRefused:
            case ConnectionStatus::Disconnected:
                text = glyph ("\xe2\x9c\x95  ") + "No bridge detected.\nStart your local generation bridge and click Test Connection again."; c = colours::bad; break;
            default:
                text = glyph ("\xe2\x9c\x95  ") + toDisplayString (conn.status) + (conn.detail.isNotEmpty() ? "\n" + conn.detail : juce::String()); c = colours::bad; break;
        }
        message.setText (text, juce::dontSendNotification);
        message.setColour (juce::Label::textColourId, c);
        done.setButtonText (conn.ok() ? "Finish" : "Finish anyway");
        if (conn.ok())
            stylePrimary (done);
        else
            styleQuiet (done);
    }
    resized();
    repaint();
}

void Onboarding::paint (juce::Graphics& g)
{
    // Same panel as every other sheet in Context.
    g.fillAll (colours::background.withAlpha (0.94f));
    const auto panel = card.toFloat();
    for (int i = 0; i < 6; ++i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.05f));
        g.fillRoundedRectangle (panel.expanded ((float) i * 2.0f).translated (0.0f, 4.0f), radius::panel + (float) i * 2.0f);
    }
    g.setColour (colours::panel);
    g.fillRoundedRectangle (panel, radius::panel + 1.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (panel.reduced (0.5f), radius::panel + 1.0f, 1.0f);

    const int s = step();
    auto r = card.reduced (28, 22);
    // Step dots: the setup really is a sequence of two.
    auto dots = r.removeFromTop (18);
    for (int i = 1; i <= 2; ++i)
    {
        g.setColour (i <= s ? colours::accent : colours::border);
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) dots.getX() + (float) (i - 1) * 22.0f, (float) dots.getCentreY() - 1.5f, 16.0f, 3.0f), 1.5f);
    }
    g.setColour (colours::faint);
    g.setFont (ui::font (type::label, Weight::Medium));
    g.drawText ("Step " + juce::String (s) + " of 2", dots.withTrimmedLeft (52), juce::Justification::centredLeft);
    g.setColour (colours::text);
    g.setFont (ui::font (24.0f, Weight::SemiBold));
    g.drawText (s == 1 ? "Activate Context" : "Connect your generation bridge", r.removeFromTop (36), juce::Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (ui::font (type::body + 1.0f));
    g.drawFittedText (s == 1 ? "Enter the license key from your purchase email."
                             : "Context sends prompts to a generation bridge running on this computer.",
                      r.removeFromTop (22), juce::Justification::centredLeft, 2);
    g.setColour (colours::dim);
    g.setFont (labelFont());
    g.drawText (s == 1 ? "License key" : "Bridge address", r.withHeight (18).translated (0, 14), juce::Justification::centredLeft);
}

void Onboarding::resized()
{
    card = getLocalBounds().withSizeKeepingCentre (juce::jmin (560, getWidth() - 40), 340);
    auto r = card.reduced (28, 22);
    r.removeFromTop (18 + 36 + 22 + 34);
    auto row = r.removeFromTop (42);
    if (step() == 1)
    {
        activate.setBounds (row.removeFromRight (120));
        row.removeFromRight (8);
        key.setBounds (row);
    }
    else
    {
        test.setBounds (row.removeFromRight (150));
        row.removeFromRight (8);
        address.setBounds (row);
    }
    r.removeFromTop (12);
    auto bottom = r.removeFromBottom (36);
    if (step() == 1)
        skip.setBounds (bottom.removeFromRight (140));
    else
    {
        done.setBounds (bottom.removeFromRight (140));
        bottom.removeFromRight (8);
        useMock.setBounds (bottom.removeFromLeft (250));
    }
    r.removeFromBottom (8);
    message.setBounds (r);
}
} // namespace context::ui
