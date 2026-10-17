#include "AtomicFile.h"

namespace context::atomicfile
{
bool writeData (const juce::File& target, const void* data, size_t size)
{
    if (! target.getParentDirectory().createDirectory())
        return false;

    juce::TemporaryFile temp (target, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk())
            return false;
        if (size > 0 && ! out.write (data, size))
            return false;
        out.flush();
        if (out.getStatus().failed())
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

bool writeText (const juce::File& target, const juce::String& text)
{
    const auto utf8 = text.toStdString();
    return writeData (target, utf8.data(), utf8.size());
}
} // namespace context::atomicfile
