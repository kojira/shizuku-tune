#pragma once

#include "KeyDetector.h"
#include "NoteSegmenter.h"
#include "PitchTracker.h"
#include "TempoDetector.h"

#include <string>
#include <vector>

namespace shizuku
{

struct NoteWithKey
{
    SungNote note;
    MusicalKey keyAtNote;     // local key (from the accompaniment) at the note's midpoint
    int nearestScaleMidi = 0; // correction target: nearest note in that key's scale, or the reference note
    bool hasReferenceTarget = false; // nearestScaleMidi comes from a reference melody (see ReferenceMelody.h)
    bool largeReferenceShift = false; // reference target farther than the near limit (allowed by high confidence)
    bool referenceUnsure = false;     // a reference note was there but too far / too unsure: scale target kept, marked
    int referenceCandidate = -1;      // index of the reference candidate used (see ReferenceSelection.h)
    int referencePart = -1;           // part of that candidate's melody (e.g. one singer of a duet)
    double referenceConfidence = 0.0; // confidence of the reference note used (0..1)
    int referenceShiftBeats = 0;      // the reference was used this many beats later (call and response), 0: as aligned

    // Reference-aligned parts (see alignNotesToReferences): the index of the reference note (in the melody of the
    // phrase's chosen candidate) this part of the singing aimed at; -1: not aligned (note from the segmenter);
    // -2: realign by time (a part made by a hand split / merge: the reference note sounding longest during it).
    long alignedReferenceNote = -1;
    // Set by applySelectedReferences for aligned parts with a reference target: the correction follows a target
    // pitch curve built from the reference notes (time-varying shift) instead of one constant shift per note.
    bool referenceCurve = false;
    // For notes with a reference target (measureReach): fraction of the voiced, vibrato-free pitch within
    // reachToleranceCents of the target, and the time from the note start until it first gets there
    // (-1: never / not measured).
    double reachFraction = -1.0;
    double settleSeconds = -1.0;
};

struct SongAnalysis
{
    bool hasAccompaniment = false;
    KeyAnalysis key;
    TempoEstimate tempo; // of the accompaniment (beat length for call-and-response reference shifts)
    bool hasVocal = false;
    PitchTrack pitch;
    std::vector<NoteWithKey> notes;
};

// Runs key detection on the accompaniment and pYIN + note segmentation on the vocal.
// Either input may be empty.
SongAnalysis analyseSong (const std::vector<float>& vocalMono, double vocalSampleRate,
                          const std::vector<float>& accompanimentMono, double accompanimentSampleRate);

MusicalKey keyAtTime (const KeyAnalysis& analysis, double timeSeconds);

// JSON document with the global key, key segments and the note list (pitch frames optional).
std::string toJson (const SongAnalysis& analysis, bool includePitchFrames);

} // namespace shizuku
