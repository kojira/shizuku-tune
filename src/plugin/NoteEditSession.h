#pragma once

#include "NoteEdits.h"
#include "ReferenceSet.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace shizuku
{

// Hand edits in the plug-in, shared by the capture session and the ARA document controller. The edit document
// (NoteEdits.h) is kept in host timeline seconds, so the edits find their notes again by time after a new capture
// or a new analysis (analysis time = timeline time - TrackAnalysisResult::timelineStartSeconds).

// Undo / redo of whole edit documents. Message thread only.
class NoteEditHistory
{
public:
    const NoteEditDocument& current() const noexcept { return document; }
    // Makes next the current document (one undo step). Returns false when nothing changed.
    bool apply (const NoteEditDocument& next);
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    // Replaces the document without an undo step (restoring a saved state) and clears the history.
    void reset (const NoteEditDocument& next);

private:
    NoteEditDocument document;
    std::vector<NoteEditDocument> undoStack, redoStack;
};

// Same edits (JSON text, ignoring "file", "baseVersion", "created" and "version").
bool sameNoteEdits (const NoteEditDocument& a, const NoteEditDocument& b);
bool hasNoteEdits (const NoteEditDocument& document);

// The correction with the hand edits, on top of the automatic correction of the same targets.
struct EditedCorrection
{
    std::shared_ptr<const CorrectionCurve> curve;          // parallel to notes
    std::shared_ptr<const std::vector<NoteWithKey>> notes; // after the edits' splits / merges
    size_t unmatchedEdits = 0, unmatchedStructure = 0;
};

// timelineEdits in timeline seconds; targets.analysis in analysis time starting at timelineStartSeconds.
// New notes made by splits / merges get their targets from the same references as the automatic targets.
// Without edits, curve / notes are the automatic ones (automaticCurve, targets.analysis.notes).
EditedCorrection planEditedCorrection (const ReferenceTargets& targets, const CorrectionSettings& settings, const NoteEditDocument& timelineEdits,
                                       double timelineStartSeconds, std::shared_ptr<const CorrectionCurve> automaticCurve,
                                       std::shared_ptr<const std::vector<NoteWithKey>> automaticNotes);

// Corrected audio of one input: the automatic render, and on top of it (when the edited curve differs) the
// spans around hand-edited notes rendered again (renderEditedSpans). Immutable once made.
struct LayeredRender
{
    std::shared_ptr<const CorrectionCurve> automaticCurve, editedCurve;
    std::shared_ptr<const std::vector<std::vector<float>>> automatic;
    std::shared_ptr<const EditedRender> edited; // null: no edits (the output is the automatic render)

    const std::vector<std::vector<float>>& output() const noexcept { return edited != nullptr ? edited->output : *automatic; }
};

// Renders input (sample 0 at analysis time sampleZeroSeconds) with automaticCurve, then the edited spans of
// editedCurve. previous (optional, for the same input): its automatic render is reused when made with the same
// automaticCurve object, and its unchanged edited spans are copied. The output equals shizuku-correct --edits.
LayeredRender renderLayered (const std::vector<std::vector<float>>& input, double sampleRate, double sampleZeroSeconds, double fundamentalHintHz,
                             std::shared_ptr<const CorrectionCurve> automaticCurve, std::shared_ptr<const CorrectionCurve> editedCurve,
                             const LayeredRender* previous);

// A note as the editor addresses it: its time span in timeline seconds.
struct NoteSpan
{
    double startSeconds = 0.0, endSeconds = 0.0;
};

// Edit operations: each returns the new document (the input with the change appended; times rounded to whole
// milliseconds as in the JSON file). Earlier edits of the same note (middle inside the note) are replaced.
NoteEditDocument withNoteTargets (const NoteEditDocument& document, const std::vector<NoteSpan>& notes, const std::vector<double>& targetMidi);
NoteEditDocument withNoteMode (const NoteEditDocument& document, const std::vector<NoteSpan>& notes, NoteEditMode mode);
NoteEditDocument withSplit (const NoteEditDocument& document, double timeSeconds);
NoteEditDocument withMerge (const NoteEditDocument& document, const NoteSpan& first, const NoteSpan& last);

// "D5 +3" (note name, octave, cents; cents optional, also "D5+3", "C#4 -12", "Eb4"). Returns false if unreadable.
bool parsePitchText (const juce::String& text, double& midi);
// "D5 +3" / "D5 -12" / "D5 +0" (whole cents).
juce::String pitchText (double midi);

} // namespace shizuku
