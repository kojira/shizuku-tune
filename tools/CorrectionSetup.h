#pragma once

// Shared by shizuku-correct and shizuku-edit-session: analysis + reference selection of one vocal take, and
// the text report (same lines in both tools).

#include "../src/analysis/ReferenceSelection.h"
#include "../src/analysis/SongAnalyzer.h"
#include "../src/analysis/WavFile.h"
#include "../src/correction/NoteEdits.h"
#include "../src/correction/PitchCorrection.h"

#include <string>
#include <vector>

namespace shizuku::tools
{

struct ReferenceArgument
{
    std::string path;
    bool offsetGiven = false;
    double offset = 0.0;
};

// "melody.svp@-0.4" -> path + offset; without "@" the offset is found automatically.
ReferenceArgument parseReferenceArgument (const std::string& text);

struct PreparedCorrection
{
    WavData vocal;
    SongAnalysis analysis; // notes aligned to the chosen references, targets set from them
    std::string log;       // reference / tempo / candidate lines (and the per-phrase choice with describePhrases)
    // Kept for retargetNotes (notes changed by hand splits / merges).
    std::vector<ReferenceCandidate> candidates;
    ReferenceSelection selection;
    ReferenceSelectionSettings selectionSettings;
};

// Reads the vocal and the accompaniment ("-" = none), analyses them and applies the reference candidates.
// choose: -2 automatic per phrase, N: candidate N; bpmOverride < 0: tempo of the accompaniment.
PreparedCorrection prepareCorrection (const std::string& vocalPath, const std::string& accompanimentPath,
                                      const std::vector<ReferenceArgument>& references, int choose, double bpmOverride,
                                      bool describePhrases);

// The analysis with the hand edits applied: the document's splits / merges (applyNoteStructure, targets set
// again with retargetNotes) and the per-note overrides (resolveNoteEdits) on the resulting notes.
struct EditedAnalysis
{
    SongAnalysis analysis;
    std::vector<NoteOverride> overrides; // parallel to analysis.notes
    std::vector<size_t> unmatchedStructure, unmatchedEdits;
};
EditedAnalysis applyEdits (const PreparedCorrection& prepared, const NoteEditDocument& document);

// "key G major, 137 notes, 106 corrected (strength 100%, threshold 25 cents) -> out.wav"
std::string describeSummary (const SongAnalysis& analysis, const CorrectionCurve& curve, const CorrectionSettings& settings,
                             const std::string& outputName);

// One line per note (time, centre, target, origin, deviation, shift, kept / follows; "(curve)" for notes corrected
// along the target pitch curve, "reach 45% settle 0.120s" for notes with a reference target). Hand-edited notes end in
// "(edited: target 71.03)" or "(edited: keep)"; with automatic (the curve without edits, for the notes
// automaticNotes, matched by time) also "; auto B4 +45.7c" (the automatic target and shift). Notes made by a split
// or merge end in "(split/merged)".
std::string describeNotes (const SongAnalysis& analysis, const CorrectionCurve& curve, const CorrectionCurve* automatic = nullptr,
                           const std::vector<NoteWithKey>* automaticNotes = nullptr);

} // namespace shizuku::tools
