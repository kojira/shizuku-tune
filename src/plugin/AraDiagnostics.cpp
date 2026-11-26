#include "AraDiagnostics.h"

#include <unistd.h>
#include <atomic>
#include <map>
#include <mutex>

namespace shizuku
{

namespace
{
std::mutex& lock()
{
    static std::mutex m;
    return m;
}
std::map<std::string, int>& counters()
{
    static std::map<std::string, int> c;
    return c;
}
} // namespace

void araDiagnostic (const juce::String& event)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/ShizukuTune");
    dir.createDirectory();
    const auto line = juce::Time::getCurrentTime().toISO8601 (true) + " pid " + juce::String ((int) ::getpid())
                      + " " + juce::File::getSpecialLocation (juce::File::hostApplicationPath).getFileName() + ": " + event + "\n";
    std::scoped_lock guard (lock());
    dir.getChildFile ("ara.log").appendText (line, false, false, "\n");
}

void araNote (const char* counter)
{
    {
        std::scoped_lock guard (lock());
        ++counters()[counter];
    }
    araDiagnostic (counter);
}

juce::String araDiagnosticSummary()
{
    std::scoped_lock guard (lock());
    juce::StringArray parts;
    for (const auto* name : { "factoryRequested", "documentControllerCreated", "bound" })
    {
        const auto it = counters().find (name);
        parts.add (juce::String (name) + " " + juce::String (it == counters().end() ? 0 : it->second));
    }
    return parts.joinIntoString (", ");
}

} // namespace shizuku
