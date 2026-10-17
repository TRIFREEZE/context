// Test runner for Context.
//   ContextTests --unit                         run all unit tests
//   ContextTests --category=Prompt              run one category
//   ContextTests --integration=<bridgeUrl>      run bridge integration tests against a live bridge
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "TestSupport.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;  // message manager for components / change broadcasters
    juce::ArgumentList args (argc, argv);

    context::test::ensureIsolatedAppDataDir();

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (args.containsOption ("--integration"))
    {
        context::test::integrationBridgeUrl() = args.getValueForOption ("--integration");
        for (auto opt : { "--wrong-protocol", "--no-generate", "--closed" })
            if (args.containsOption (opt))
                context::test::integrationUrls().set (juce::String (opt).substring (2), args.getValueForOption (opt));
        runner.runTestsInCategory ("Integration");
    }
    else if (args.containsOption ("--license-server"))
    {
        for (auto opt : { "--license-server", "--license-key", "--license-key-single", "--license-key-revoke",
                          "--license-public-key", "--license-db", "--backend-dir", "--python" })
            if (args.containsOption (opt))
                context::test::integrationUrls().set (juce::String (opt).substring (2), args.getValueForOption (opt));
        runner.runTestsInCategory ("LicenseIntegration");
    }
    else if (args.containsOption ("--category"))
    {
        runner.runTestsInCategory (args.getValueForOption ("--category"));
    }
    else
    {
        juce::Array<juce::UnitTest*> tests;
        for (auto* t : juce::UnitTest::getAllTests())
            if (t->getCategory() != "Integration" && t->getCategory() != "LicenseIntegration")
                tests.add (t);
        runner.runTests (tests);
    }

    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }

    std::printf ("\n==== %d checks passed, %d failed ====\n", passes, failures);
    juce::DeletedAtShutdown::deleteAll();
    return failures == 0 ? 0 : 1;
}
