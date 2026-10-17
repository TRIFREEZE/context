#include "EffectProcessor.h"

namespace context::fx
{
EffectProcessor::EffectProcessor (juce::String typeId, juce::String displayName)
    : type (std::move (typeId)), name (std::move (displayName))
{
}

EffectParameter& EffectProcessor::addParameter (ParameterSpec spec)
{
    params.push_back (std::make_unique<EffectParameter> (std::move (spec)));
    return *params.back();
}

EffectParameter* EffectProcessor::parameter (const juce::String& paramId) const noexcept
{
    for (auto& p : params)
        if (p->id() == paramId)
            return p.get();
    return nullptr;
}

juce::var EffectProcessor::getParameterState() const
{
    auto* o = new juce::DynamicObject();
    for (auto& p : params)
        o->setProperty (p->id(), p->get());
    return juce::var (o);
}

void EffectProcessor::setParameterState (const juce::var& v)
{
    if (auto* o = v.getDynamicObject())
        for (auto& p : params)
            if (o->hasProperty (p->id()))
            {
                const auto& value = o->getProperty (p->id());
                if (value.isDouble() || value.isInt() || value.isInt64() || value.isBool())
                    p->set ((float) (double) value);
            }
}

void EffectProcessor::resetParameters() noexcept
{
    for (auto& p : params)
        p->reset();
}
} // namespace context::fx
