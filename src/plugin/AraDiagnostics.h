#pragma once

#include <juce_core/juce_core.h>

namespace shizuku
{

// Small diagnostic trail for "does the host use us as ARA?": counts of the ARA entry points the host
// called, plus one line per event in ~/Library/Logs/ShizukuTune/ara.log (timestamped, process id).
void araDiagnostic (const juce::String& event);
void araNote (const char* counter); // factoryRequested / documentControllerCreated / bound / editorView
juce::String araDiagnosticSummary();

} // namespace shizuku
