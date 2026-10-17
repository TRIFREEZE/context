#include "EffectRegistry.h"

namespace context::fx
{
void EffectRegistry::registerEffect (EffectType t)
{
    jassert (t.id.isNotEmpty() && t.create != nullptr);
    jassert (find (t.id) == nullptr);   // ids must be unique
    if (t.id.isEmpty() || t.create == nullptr || find (t.id) != nullptr)
        return;
    all.push_back (std::move (t));
}

const EffectType* EffectRegistry::find (const juce::String& id) const noexcept
{
    for (auto& t : all)
        if (t.id == id)
            return &t;
    return nullptr;
}

std::unique_ptr<EffectProcessor> EffectRegistry::create (const juce::String& id) const
{
    if (auto* t = find (id))
        return t->create();
    return nullptr;
}

juce::StringArray EffectRegistry::categories() const
{
    juce::StringArray c;
    for (auto& t : all)
        c.addIfNotAlreadyThere (t.category);
    return c;
}

const EffectRegistry& EffectRegistry::builtIn()
{
    static const EffectRegistry registry = []
    {
        EffectRegistry r;
        registerBuiltInEffects (r);
        return r;
    }();
    return registry;
}
} // namespace context::fx
