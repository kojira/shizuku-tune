#pragma once

#include "PitchCorrection.h"
#include "PitchLineView.h"
#include "ReferenceSet.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace shizuku
{

// What the editor's shared controls (strength / threshold, reference candidates and choice, status, pitch view)
// work on: the ARA document controller or the capture session. Message thread only; change messages are sent
// whenever any of it changes.
class CorrectionModel : public juce::ChangeBroadcaster
{
public:
    ~CorrectionModel() override = default;

    // Correction strength (0..1) and threshold (cents). Changing them re-renders the corrected audio.
    virtual CorrectionSettings getCorrectionSettings() const = 0;
    virtual void setCorrectionSettings (const CorrectionSettings& newSettings) = 0;

    // Reference melody candidates; returns an error message on failure.
    virtual juce::String addReferenceMelody (const juce::File& file) = 0;
    virtual void clearReferenceMelodies() = 0;
    virtual juce::String getReferenceDescription() const = 0;
    // referenceAuto / referenceNone / candidate index (see ReferenceSet.h).
    virtual int getReferenceChoice() const = 0;
    virtual void setReferenceChoice (int choice) = 0;
    virtual bool canChooseReference() const = 0;
    virtual std::vector<ReferenceCandidateInfo> getReferenceCandidates() const = 0;

    virtual juce::String getStatusText() const = 0;
    virtual std::shared_ptr<const TrackAnalysisResult> getLatestResult() const = 0;
    virtual std::shared_ptr<const CorrectionCurve> getLatestCorrection() const = 0;
    // Analysed notes with the targets actually used (scale or reference), parallel to getLatestCorrection()->notes.
    virtual std::shared_ptr<const std::vector<NoteWithKey>> getLatestTargets() const = 0;
};

} // namespace shizuku
