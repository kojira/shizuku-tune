#pragma once

#include "PitchCorrection.h"
#include "ReferenceSelection.h"

#include <string>
#include <vector>

namespace shizuku
{

// Hand edits of single notes ("手直し"), shared by the command-line tools and the plug-in.
// A note is identified by its time span, so an edit still finds its note when the automatic note segmentation
// moves the boundaries a little (see resolveNoteEdits).
//
// JSON file (e.g. passed to shizuku-correct --edits):
// {
//   "format": "shizuku-note-edits", "formatVersion": 2,
//   "file": "vocal.wav",                   // vocal file name
//   "baseVersion": "v1",                   // automatic correction the edits were made on (informational)
//   "structure": [                         // splits and merges of notes, applied in order before the edits
//     { "op": "split", "time": 23.612 },   // the note sounding at this time is split there into two notes
//     { "op": "merge", "start": 23.27, "end": 23.75 }, // the notes whose middle lies in the span become one
//     ... each with "created" / "version" like an edit
//   ],
//   "edits": [
//     { "start": 35.129, "end": 35.738,    // time span of the note when the edit was made (seconds)
//       "mode": "target",                  // "target" | "keep" (no correction) | "auto" (automatic decision)
//       "targetMidi": 71.03,               // "target" only: fractional MIDI, 0.01 = 1 cent
//       "created": "2026-01-01T10:00:00+09:00", "version": "v1" }
//   ]
// }
// formatVersion 1 files (no "structure") are read as they are. Later entries win over earlier ones for the same
// note. Unknown members are ignored (a front end may keep its own data, e.g. an undo history, in the same file).
struct NoteEdit
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    NoteEditMode mode = NoteEditMode::target;
    double targetMidi = 0.0;
    std::string created;
    std::string version;
};

// A split or merge of notes ("structure" in the JSON). Splits and merges find their notes by time as well, so they
// survive a new automatic analysis (see applyNoteStructure).
struct NoteStructureEdit
{
    enum class Op
    {
        split, // split the note sounding at timeSeconds there
        merge  // join the notes whose middle lies in [startSeconds, endSeconds] into one note
    };
    Op op = Op::split;
    double timeSeconds = 0.0;
    double startSeconds = 0.0, endSeconds = 0.0;
    std::string created;
    std::string version;
};

struct NoteEditDocument
{
    std::string file;
    std::string baseVersion;
    std::vector<NoteStructureEdit> structure;
    std::vector<NoteEdit> edits;
};

NoteEditDocument parseNoteEdits (const std::string& jsonText); // throws std::runtime_error
NoteEditDocument loadNoteEdits (const std::string& path);      // throws std::runtime_error
std::string toJson (const NoteEditDocument& document);

// Target MIDI rounded to whole cents (the precision of an edit).
double roundToCent (double midi);

struct NoteEditMatchSettings
{
    // Without any time overlap, the nearest note (gap between the note and the edit's span) is used if the gap
    // is smaller than this.
    double maxGapSeconds = 0.15;
    // Besides the best-overlapping note, every note lying at least this fraction inside the edit's span gets
    // the edit as well (one edited note re-segmented into two).
    double splitNoteInsideFraction = 0.5;
};

struct NoteStructureSettings
{
    double minPartSeconds = 0.03;     // a split leaving a shorter part (or no voiced frame) on either side is not applied
    double existingBoundarySeconds = 0.02; // a split this close to an existing note boundary is already there
};

// Applies the splits and merges in order to notes (sorted by time). New notes are measured from the pitch track
// (noteFromFrames) and get keyAtNote / a scale target from analysis, and alignedReferenceNote = -2 (their reference
// note is found again by time); every other note is left as it was. unmatched (optional): indices of structure
// edits that found no note to act on (a split already at a boundary, or a merge of a single note, counts as applied).
std::vector<NoteWithKey> applyNoteStructure (const SongAnalysis& analysis, const std::vector<NoteWithKey>& notes,
                                             const std::vector<NoteStructureEdit>& structure, const NoteStructureSettings& settings = {},
                                             std::vector<size_t>* unmatched = nullptr);

// One override per note (parallel to notes): the edits applied in order, later ones replacing earlier ones.
// unmatched (optional) receives the indices of edits that found no note.
std::vector<NoteOverride> resolveNoteEdits (const std::vector<NoteWithKey>& notes, const std::vector<NoteEdit>& edits,
                                            const NoteEditMatchSettings& settings = {}, std::vector<size_t>* unmatched = nullptr);

// The document with every time moved by offsetSeconds (e.g. from host timeline seconds to analysis time).
NoteEditDocument shiftNoteEdits (const NoteEditDocument& document, double offsetSeconds);

// Notes with the hand edits applied: the document's splits / merges (applyNoteStructure, then the targets set
// again with retargetNotes from candidates / selection; no candidates: scale targets) and the per-note overrides
// (resolveNoteEdits) on the resulting notes. Without structure edits the notes are analysis.notes unchanged.
struct EditedNotes
{
    std::vector<NoteWithKey> notes;
    std::vector<NoteOverride> overrides; // parallel to notes
    std::vector<size_t> unmatchedStructure, unmatchedEdits;
};
EditedNotes applyNoteEdits (const SongAnalysis& analysis, const NoteEditDocument& document, const std::vector<ReferenceCandidate>& candidates,
                            const ReferenceSelection& selection, const ReferenceSelectionSettings& selectionSettings);

} // namespace shizuku
