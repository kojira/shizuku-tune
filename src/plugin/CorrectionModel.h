#pragma once

#include "NoteEditSession.h"
#include "PitchCorrection.h"
#include "PitchLineView.h"
#include "ReferenceSet.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace shizuku
{

// Vocal audio of a timeline range, for auditioning in the editor (message thread).
struct AuditionAudio
{
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;
};

// What the editor's shared controls (strength / threshold, reference candidates and choice, status, pitch view,
// hand edits) work on: the ARA document controller or the capture session. Message thread only; change messages
// are sent whenever any of it changes.
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
    // The correction with the hand edits, and the notes it was planned for (after the edits' splits / merges;
    // targets actually used: scale or reference), parallel to each other.
    virtual std::shared_ptr<const CorrectionCurve> getLatestCorrection() const = 0;
    virtual std::shared_ptr<const std::vector<NoteWithKey>> getLatestTargets() const = 0;
    // The same without hand edits (the automatic decision shown in the edit panel).
    virtual std::shared_ptr<const CorrectionCurve> getAutomaticCorrection() const = 0;
    virtual std::shared_ptr<const std::vector<NoteWithKey>> getAutomaticTargets() const = 0;
    // The references as applied (candidates with offsets and the per-phrase choice), for drawing; may be null.
    virtual std::shared_ptr<const ReferenceTargets> getReferenceTargets() const = 0;
    // True while the corrected audio is being rendered again (the previous one keeps playing).
    virtual bool isRendering() const = 0;
    // The vocal (corrected: as it plays now, else the input) for timeline seconds [startSeconds, endSeconds);
    // false when there is no audio there.
    virtual bool getAuditionAudio (double startSeconds, double endSeconds, bool corrected, AuditionAudio& audio) const = 0;

    // Hand edits (NoteEdits.h, times in host timeline seconds), with undo / redo of whole operations.
    const NoteEditDocument& getNoteEdits() const noexcept { return noteEditHistory.current(); }
    void applyNoteEdits (const NoteEditDocument& next)
    {
        if (noteEditHistory.apply (next))
            noteEditsChanged();
    }
    void undoNoteEdit()
    {
        if (noteEditHistory.undo())
            noteEditsChanged();
    }
    void redoNoteEdit()
    {
        if (noteEditHistory.redo())
            noteEditsChanged();
    }
    bool canUndoNoteEdit() const noexcept { return noteEditHistory.canUndo(); }
    bool canRedoNoteEdit() const noexcept { return noteEditHistory.canRedo(); }

protected:
    // Saves the edits and re-renders only around the edited notes.
    virtual void noteEditsChanged() = 0;

    NoteEditHistory noteEditHistory;
};

} // namespace shizuku
