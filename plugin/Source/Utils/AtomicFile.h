#pragma once

#include <juce_core/juce_core.h>

namespace context::atomicfile
{
/** Writes text to a temporary file next to `target`, flushes it, then atomically
    replaces `target` (MoveFileEx/rename). Readers never observe a half-written file.
    Returns false on any failure (the old file is left untouched). */
bool writeText (const juce::File& target, const juce::String& text);

/** Same for binary data. */
bool writeData (const juce::File& target, const void* data, size_t size);
} // namespace context::atomicfile
