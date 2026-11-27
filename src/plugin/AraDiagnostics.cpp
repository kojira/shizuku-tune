#include "AraDiagnostics.h"

#include <atomic>
#include <map>
#include <mutex>

#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <unistd.h>
#endif

namespace shizuku
{

namespace
{
std::mutex& lock()
{
    static std::mutex m;
    return m;
}
int processId()
{
   #if JUCE_WINDOWS
    return ::_getpid();
   #else
    return (int) ::getpid();
   #endif
}

// macOS: ~/Library/Logs/ShizukuTune; elsewhere: <user application data>/ShizukuTune/logs (%APPDATA% on Windows).
juce::File logDirectory()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/ShizukuTune");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ShizukuTune/logs");
   #endif
}

std::map<std::string, int>& counters()
{
    static std::map<std::string, int> c;
    return c;
}
} // namespace

void araDiagnostic (const juce::String& event)
{
    const auto dir = logDirectory();
    dir.createDirectory();
    const auto line = juce::Time::getCurrentTime().toISO8601 (true) + " pid " + juce::String (processId())
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
