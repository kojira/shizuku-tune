#pragma once

#include <juce_core/juce_core.h>

namespace shizuku
{

// Small diagnostic trail for "does the host use us as ARA?": counts of the ARA entry points the host
// called, plus one line per event in ara.log (timestamped, process id) in ~/Library/Logs/ShizukuTune on macOS and
// %APPDATA%/ShizukuTune/logs on Windows.
void araDiagnostic (const juce::String& event);
void araNote (const char* counter); // factoryRequested / documentControllerCreated / bound / editorView
juce::String araDiagnosticSummary();

} // namespace shizuku
