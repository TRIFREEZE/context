#include "Motion.h"

namespace context::ui::motion
{
namespace
{
    std::atomic<bool> motionOn { true }, motionAllowed { true };
}

bool enabled() noexcept           { return motionOn.load() && motionAllowed.load(); }
void setEnabled (bool b) noexcept { motionOn.store (b); }
void setAllowed (bool b) noexcept { motionAllowed.store (b); }
double nowMs() noexcept         { return juce::Time::getMillisecondCounterHiRes(); }

float apply (Ease e, float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);
    switch (e)
    {
        case Ease::Linear:     return t;
        case Ease::OutCubic:   { const float u = 1.0f - t; return 1.0f - u * u * u; }
        case Ease::InOutCubic: return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow (-2.0f * t + 2.0f, 3.0f) * 0.5f;
        case Ease::OutQuint:   { const float u = 1.0f - t; return 1.0f - u * u * u * u * u; }
        case Ease::OutBack:    { const float c1 = 1.20158f, c3 = c1 + 1.0f, u = t - 1.0f; return 1.0f + c3 * u * u * u + c1 * u * u; }
    }
    return t;
}

// ---------------------------------------------------------------------------

float follow (juce::Component& c, const juce::Identifier& key, float target, float ms, Ease ease)
{
    auto& props = c.getProperties();
    // Each animated property keeps: target, start value, start time, length.
    const juce::Identifier kT (key.toString() + "_t"), kF (key.toString() + "_f"), kS (key.toString() + "_s"), kD (key.toString() + "_d");
    const double now = nowMs();
    if (! enabled() || ms <= 0.0f)
    {
        // Jump (and settle, so a later animated change starts from here).
        props.set (kT, target);
        props.set (kF, target);
        props.set (kD, 0.0);
        return target;
    }
    if (! props.contains (kT))
    {
        props.set (kT, target);
        props.set (kF, target);
        props.set (kS, 0.0);
        props.set (kD, 0.0);
        return target;
    }
    auto valueAt = [&] (double t)
    {
        const float to = (float) props[kT], from = (float) props[kF];
        const double s = props[kS], d = props[kD];
        if (d <= 0.0 || t >= s + d) return to;
        return from + (to - from) * apply (ease, (float) ((t - s) / d));
    };
    if (std::abs ((float) props[kT] - target) > 1.0e-5f)
    {
        const float current = valueAt (now);
        props.set (kF, current);
        props.set (kT, target);
        props.set (kS, now);
        props.set (kD, (double) ms);
        Clock::instance().repaintUntil (c, now + ms);
    }
    return valueAt (now);
}

float hoverAmount (juce::Component& c, bool over)
{
    static const juce::Identifier key ("mHover");
    return follow (c, key, over ? 1.0f : 0.0f, over ? duration::hover : duration::hover * 1.6f);
}

float pressAmount (juce::Component& c, bool down)
{
    static const juce::Identifier key ("mPress");
    return follow (c, key, down ? 1.0f : 0.0f, down ? duration::press : duration::hover * 1.4f);
}

// ---------------------------------------------------------------------------

Animated::~Animated()
{
    if (running)
        if (auto* c = Clock::existing())
            c->remove (this);
}

void Animated::startAnimating()
{
    if (! running)
    {
        running = true;
        Clock::instance().add (this);
    }
}

void Animated::stopAnimating()
{
    if (running)
    {
        running = false;
        if (auto* c = Clock::existing())
            c->remove (this);
    }
}

void Value::set (float target, float ms, Ease e)
{
    if (! enabled() || ms <= 0.0f)
    {
        snap (target);
        return;
    }
    if (std::abs (target - to) < 1.0e-6f && isAnimating())
        return;
    if (std::abs (target - current) < 1.0e-6f)
    {
        snap (target);
        return;
    }
    from = current;
    to = target;
    start = nowMs();
    length = ms;
    ease = e;
    startAnimating();
}

void Value::snap (float v)
{
    stopAnimating();
    const bool changed = std::abs (v - current) > 1.0e-6f;
    current = from = to = v;
    if (changed && onChange)
        onChange();
}

bool Value::tick (double now)
{
    const float t = length > 0.0f ? (float) ((now - start) / length) : 1.0f;
    current = t >= 1.0f ? to : from + (to - from) * apply (ease, t);
    if (onChange)
        onChange();
    return t < 1.0f;
}

void Rect::set (juce::Rectangle<float> r, float ms, Ease e)
{
    if (! initialised)
    {
        snap (r);
        return;
    }
    x.set (r.getX(), ms, e);
    y.set (r.getY(), ms, e);
    w.set (r.getWidth(), ms, e);
    h.set (r.getHeight(), ms, e);
}

void Rect::snap (juce::Rectangle<float> r)
{
    initialised = true;
    x.snap (r.getX());
    y.snap (r.getY());
    w.snap (r.getWidth());
    h.snap (r.getHeight());
}

juce::Rectangle<float> Rect::get() const { return { x.get(), y.get(), w.get(), h.get() }; }

// ---------------------------------------------------------------------------

Clock* Clock::instancePtr = nullptr;

Clock& Clock::instance()
{
    // Message thread only. Deleted by JUCE at shutdown (DeletedAtShutdown), so its timer
    // never outlives the plugin.
    if (instancePtr == nullptr)
        instancePtr = new Clock();
    return *instancePtr;
}

Clock* Clock::existing() { return instancePtr; }

Clock::~Clock()
{
    stopTimer();
    for (auto* a : items)
        a->running = false;
    instancePtr = nullptr;
}

void Clock::add (Animated* a)
{
    if (std::find (items.begin(), items.end(), a) == items.end())
        items.push_back (a);
    ensureRunning();
}

void Clock::remove (Animated* a)
{
    items.erase (std::remove (items.begin(), items.end(), a), items.end());
}

void Clock::repaintUntil (juce::Component& c, double endMs)
{
    for (auto& r : repaints)
        if (r.first.getComponent() == &c)
        {
            r.second = juce::jmax (r.second, endMs);
            ensureRunning();
            return;
        }
    repaints.emplace_back (&c, endMs);
    ensureRunning();
}

std::unique_ptr<juce::VBlankAttachment> Clock::attach (juce::Component& top)
{
    return std::make_unique<juce::VBlankAttachment> (&top, [this]
    {
        lastVBlank = nowMs();
        tick();
    });
}

void Clock::ensureRunning()
{
    if (! isTimerRunning())
        startTimerHz (60);
}

void Clock::timerCallback()
{
    // The vblank drives frames while the editor is visible; the timer only fills in when
    // there is no display link (or when nothing is moving, to stop itself).
    if (nowMs() - lastVBlank > 50.0)
        tick();
    if (items.empty() && repaints.empty())
        stopTimer();
}

void Clock::tick()
{
    if (ticking)
        return;
    const juce::ScopedValueSetter<bool> guard (ticking, true);
    const double now = nowMs();
    auto snapshot = items;
    for (auto* a : snapshot)
    {
        if (std::find (items.begin(), items.end(), a) == items.end())
            continue;   // removed by an earlier callback this frame
        if (! a->tick (now))
        {
            a->running = false;
            remove (a);
        }
    }
    for (auto it = repaints.begin(); it != repaints.end();)
    {
        if (auto* c = it->first.getComponent())
            c->repaint();
        // one extra frame after the end so the final value is painted
        it = (it->first == nullptr || now > it->second + 20.0) ? repaints.erase (it) : std::next (it);
    }
}
} // namespace context::ui::motion
