#pragma once

#include "ReferenceMelody.h"

#include <string>
#include <vector>

namespace shizuku
{

// One candidate "correct melody" for a sung track (guide A, guide B, the original's lead melody, ...).
struct ReferenceCandidate
{
    std::string name;
    ReferenceMelody melody;
    double offsetSeconds = 0.0; // vocal time = reference time + offset
};

struct ReferenceSelectionSettings
{
    double phraseGapSeconds = 0.3;      // sung notes further apart than this start a new segment
    double strictToleranceSemitones = 0.6;
    double looseToleranceSemitones = 1.1; // also counts a singer ~1 semitone off the reference note
    double minSegmentMatch = 0.5;        // loose match (of the segment's sung time with that reference present)
    double minPresence = 0.3;            // the reference must sound during this fraction of the segment's sung time

    // Distance limits for using a reference note as the target.
    double nearSemitones = 2.5;           // used up to this distance (nearest octave); for estimated references
                                          // (confidence < 1, e.g. the original's lead melody) only up to
                                          // nearLimitForConfidence (see below)
    double largeSemitones = 6.5;          // ... up to this distance when the reference is trustworthy here
                                          // (estimated references, confidence < 1: only for notes of at least
    double largeMinNoteSeconds = 0.2;     //  largeMinNoteSeconds; a shorter one is more likely a scoop / transition):
    double largeMinNoteConfidence = 0.9;  // the reference note's own confidence (separation quality), and
    double largeMinNeighbourMatch = 0.5;  // the loose match of the other segments within +-neighbourSeconds
    double neighbourSeconds = 8.0;
    double minNeighbourSungSeconds = 1.0; // less singing around: the note's own segment (without the note) is used

    // Distance limit for estimated reference notes (confidence < 1) by their confidence: lowConfidenceSemitones
    // up to lowConfidence, rising linearly to nearSemitones at fullConfidence. Farther notes keep the scale target
    // near what was sung and are marked referenceUnsure. Chosen on real multi-singer takes: of the limits that
    // leave notes a listener marked "the original pitch was right" near the singing (they had been moved
    // 103-209 cents towards estimated lead notes of confidence 0.10-0.77), this one holds back the fewest other notes.
    // An estimated melody confirmed by other takes singing it can carry confidence >= 0.95 (MIDI velocity, see loadMidi).
    double lowConfidenceSemitones = 1.0;
    double lowConfidence = 0.75;
    double fullConfidence = 0.9;

    double nearLimitForConfidence (double confidence) const;

    // Call and response (echo): a phrase may follow a candidate a whole number of beats later or earlier.
    // Per phrase, shifts of +-1..maxShiftBeats beats are tried; one is used only when its loose match reaches
    // minShiftMatch and beats the best unshifted one (of any candidate) by at least minShiftGain, and its match
    // against the notes a target would use is not worse than the best unshifted one. Among those,
    // the shift (and candidate) with the best shiftScore wins: a stepwise line matches a neighbouring beat almost
    // as well in pitch, the onsets tell them apart.
    double beatSeconds = 0.0;             // 0: no shifted matching (no tempo)
    int maxShiftBeats = 4;
    double minShiftMatch = 0.7;
    double minShiftGain = 0.25;
    double minShiftSungSeconds = 0.6;     // phrases with less singing or fewer notes are not shifted
    size_t minShiftNotes = 3;
};

// Beat length to use for shifted matching from an accompaniment tempo (0 when the tempo is not reliable).
double beatSecondsForTempo (double bpm, double confidence);

// What was chosen for one sung segment (phrase).
struct ReferenceSegmentChoice
{
    double startSeconds = 0.0, endSeconds = 0.0;
    size_t firstNote = 0, endNote = 0; // note index range [firstNote, endNote)
    int candidate = -1;                // -1: none
    bool matched = false;              // the candidate reached minSegmentMatch here (false: file-level fallback)
    int octave = 0;                    // sung - reference, in octaves (most common among matching notes)
    int part = -1;                     // most common reference part of the matching notes (e.g. one singer of a duet)
    double sungSeconds = 0.0;          // total length of the segment's notes
    // Per candidate, fractions of sungSeconds, at that candidate's shift (shiftBeats; 0 = as aligned).
    std::vector<double> strictMatch, looseMatch, presence;
    std::vector<int> shiftBeats;
    std::vector<double> shiftScore; // per candidate at its shift: target-note pitch + onset agreement (0..2)
    double unshiftedBestLooseMatch = 0.0; // best loose match of any candidate without a shift

    int chosenShiftBeats() const { return candidate >= 0 ? shiftBeats[(size_t) candidate] : 0; }
};

struct ReferenceSelection
{
    double beatSeconds = 0.0; // settings.beatSeconds used for the shifts
    std::vector<ReferenceSegmentChoice> segments;
    std::vector<double> overallLooseMatch;   // per candidate, over sung time where it is present
    std::vector<double> overallPresence;     // per candidate
    int fileCandidate = -1;                  // candidate chosen by the most sung time (fallback for unmatched segments)
};

// Splits the sung notes into phrases and picks, per phrase, the candidate that matches best (loose match, any
// octave), trying whole-beat shifts of each candidate as well (see beatSeconds). Phrases where no candidate reaches minSegmentMatch fall back to the file-level candidate if it is
// present there. forcedCandidate >= 0 uses that candidate everywhere it is present; -2 = automatic.
ReferenceSelection selectReferences (const std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                     const ReferenceSelectionSettings& settings = {}, int forcedCandidate = -2);

struct ReferenceApplyStats
{
    size_t referenceTargets = 0; // notes whose target is a reference note
    size_t largeShifts = 0;      // ... of which farther than nearSemitones (trusted reference)
    size_t unsure = 0;           // reference present but too far / not trusted: scale target kept, marked
};

// Sets the target of every note covered by a chosen segment to its main reference note: the aligned one
// (alignedReferenceNote >= 0), otherwise the one it overlaps most (with the segment's beat shift applied): within nearSemitones at the nearest octave; up to largeSemitones (at the segment's octave) when the
// main note's confidence and the neighbouring segments' match are high enough. Other notes keep their target
// (marked referenceUnsure when a main note was there). Aligned parts (alignedReferenceNote != -1) with a reference
// target get referenceCurve (corrected along the target pitch curve, see planCorrection).
ReferenceApplyStats applySelectedReferences (std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                             const ReferenceSelection& selection, const ReferenceSelectionSettings& settings = {});

struct NoteAlignmentSettings
{
    // Per phrase with a chosen candidate, the sung frames are matched in time order to that candidate's notes
    // (dynamic time warping, monotonic; reference notes may be skipped). Frame cost (semitones): distance of the
    // vibrato-free pitch to the reference note (any octave, at most maxPitchCost) + timeWeightPerSecond x the time
    // outside the reference note (widened by timeSlackSeconds, beat shift applied).
    double smoothSeconds = 0.16; // vibrato-free line (smoothedPitchMidi)
    double maxPitchCost = 3.0;
    double timeWeightPerSecond = 15.0;
    double timeSlackSeconds = 0.08;
    double skipCost = 2.0;          // per reference note skipped between two sung parts
    double changeInsideNoteCost = 0.5; // moving to the next reference note inside a sung note (free at a note boundary)
    double minPartSeconds = 0.06;   // shorter runs join the neighbouring part of the same sung note
    double mergeGapSeconds = 0.04;  // consecutive parts on the same reference note closer than this become one
    double alignedMiddleSlackSeconds = 0.02; // a part counts as aimed at its reference note when its middle is inside it
};

struct NoteAlignmentStats
{
    size_t phrases = 0;       // phrases aligned
    size_t notesBefore = 0;   // segmenter notes in those phrases
    size_t partsAfter = 0;    // reference-aligned parts that replaced them
};

// Re-divides the notes of every phrase with a chosen candidate into parts, one per reference note the singer
// aimed at (NoteWithKey::alignedReferenceNote), so a note that slides through two reference notes without
// settling is split where the singing moves from one to the next, and pieces of one reference note (scoops,
// wobbles split off by the segmenter, closer than mergeGapSeconds) are joined. Longer gaps stay boundaries. The phrases' note
// ranges in selection are updated. Phrases without a candidate keep the segmenter's notes.
NoteAlignmentStats alignNotesToReferences (SongAnalysis& analysis, const std::vector<ReferenceCandidate>& candidates,
                                           ReferenceSelection& selection, const NoteAlignmentSettings& settings = {});

// Sets every phrase's note range [firstNote, endNote) from the notes' times (note midpoint inside the phrase),
// after the notes were changed (alignment, hand splits / merges).
void assignNotesToSegments (ReferenceSelection& selection, const std::vector<NoteWithKey>& notes);

// Sets the targets of notes that were changed after the selection (e.g. analysis.notes after hand splits / merges)
// as the first pass did: scale targets from analysis' key, then (with candidates) the chosen references of
// selection (phrase note ranges recomputed) and the reach. The notes' alignedReferenceNote is kept.
void retargetNotes (const SongAnalysis& analysis, std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                    const ReferenceSelection& selection, const ReferenceSelectionSettings& settings);

struct ReachSettings
{
    double smoothSeconds = 0.16;       // vibrato-free line, as for the alignment
    double toleranceSemitones = 0.5;   // "reached": within 50 cents of the target
};

// For notes with a reference target: reachFraction and settleSeconds (see NoteWithKey).
void measureReach (std::vector<NoteWithKey>& notes, const PitchTrack& pitch, const ReachSettings& settings = {});

// The correct pitch (MIDI) for each note for error measurement: the main reference note of the chosen
// candidate at the segment's octave, or at the nearest octave when that is closer by more than an octave's
// half (a segment sung across two octaves). For aligned parts the main note is the aligned reference note.
// NaN where no candidate / no main note covers half the note.
std::vector<double> referencePitchForErrors (const std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                             const ReferenceSelection& selection);

// One line per segment ("12.30-15.10s  lead.mid [lead (singer A)] +1oct +2 beats ...") for reports.
std::string describeSelection (const ReferenceSelection& selection, const std::vector<ReferenceCandidate>& candidates);

} // namespace shizuku
