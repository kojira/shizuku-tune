#pragma once

#include "PitchCorrection.h"
#include "SongAnalyzer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace shizuku
{

// Result of analysing the chosen vocal and accompaniment tracks, in host playback time.
struct TrackAnalysisResult
{
    juce::String vocalTrackName;
    juce::String accompanimentTrackName;
    double timelineStartSeconds = 0.0; // playback time of analysis time 0
    double timelineEndSeconds = 0.0;
    SongAnalysis analysis;
};

// Draws the vocal pitch line, detected notes, key regions and the corrected pitch line.
class PitchLineView final : public juce::Component
{
public:
    void setResult (std::shared_ptr<const TrackAnalysisResult> newResult);
    // Correction to draw on top: corrected pitch line, and per note the target actually used (targets is
    // parallel to curve->notes; reference targets are drawn in a different colour).
    void setCorrection (std::shared_ptr<const CorrectionCurve> newCurve, std::shared_ptr<const std::vector<NoteWithKey>> newTargets);
    void paint (juce::Graphics&) override;

private:
    std::shared_ptr<const TrackAnalysisResult> result;
    std::shared_ptr<const CorrectionCurve> curve;
    std::shared_ptr<const std::vector<NoteWithKey>> targets;
};

} // namespace shizuku
