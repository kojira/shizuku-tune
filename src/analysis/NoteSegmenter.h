#pragma once

#include "PitchTracker.h"

#include <vector>

namespace shizuku
{

struct SungNote
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double centreMidi = 0.0;  // median pitch of the stable part of the note (fractional MIDI): the middle 60 %, or
                              // for a note whose pitch moves steadily (scoop into / fall out of a held pitch) the
                              // longest run of frames within plateauWidthSemitones
    bool glide = false;       // the pitch moves through the whole note (no plateau): no stable centre
    int nearestMidi = 0;      // nearest equal-tempered note
    double centsOffset = 0.0; // centreMidi - nearestMidi, in cents
    double meanRmsDb = -120.0;
};

struct NoteSegmenterSettings
{
    double minNoteSeconds = 0.06;
    // Pitch splits, on two lines: the raw pitch and the vibrato-free line (centred running median of the voiced
    // frames over vibratoSmoothSeconds, about one vibrato period, so vibrato cancels out and steps stay steps).
    //  - Leap: the note so far ends in a held stretch (its last holdSeconds within holdWidthSemitones on the
    //    vibrato-free line), the raw pitch leaves the running note centre by >= leapSemitones for jumpConfirmSeconds, the
    //    vibrato-free line follows by at least half of that (a wide vibrato does not), and then holds a level
    //    (holdSeconds within holdWidthSemitones; a fall at the end of a note does not).
    //  - Held level: the vibrato-free line has held one level (holdSeconds within holdWidthSemitones, and flat: its
    //    first and last quarter within half of that) in the note so far, and then holds a new level (again holdSeconds within holdWidthSemitones) >= levelChangeSemitones
    //    away from it. The split is where the line crosses the middle of the two levels.
    // Movements that come back (vibrato) or do not settle (a scoop into a note before its first held level, a fall
    // at its end) do not split. Thresholds: see README ("note segmentation").
    double leapSemitones = 1.5;
    double jumpConfirmSeconds = 0.035;
    double vibratoSmoothSeconds = 0.16;
    double holdSeconds = 0.08;
    double holdWidthSemitones = 0.5;
    double levelChangeSemitones = 0.8;
    double onsetRiseDb = 9.0;          // split on a sudden level rise (re-articulated same pitch)
    double onsetWindowSeconds = 0.05;
    double maxGapSeconds = 0.03;       // short unvoiced gaps inside a note are bridged

    // Notes whose pitch trend (least-squares line) changes by >= glideMinSemitones and explains >= glideMinR2
    // of the pitch variance (vibrato does not) take their centre from the plateau; without a plateau covering
    // minPlateauFraction of the note they are glides.
    double glideMinSemitones = 1.0;
    double glideMinR2 = 0.5;
    double plateauWidthSemitones = 0.7;
    double minPlateauFraction = 0.4;
};

// Centre of one note from its voiced pitch values (MIDI, in time order), as the segmenter computes it
// (middle 60 %, or the plateau of a scoop / fall); glide is set when the pitch moves without a plateau.
double noteCentreMidi (const std::vector<double>& midiValues, const NoteSegmenterSettings& settings, bool& glide);

// One note from the voiced frames of track in [fromFrame, toFrame) (centre, nearest note, level as the segmenter
// computes them). The note spans its first to last voiced frame; nothing voiced: a note with startSeconds ==
// endSeconds.
SungNote noteFromFrames (const PitchTrack& track, size_t fromFrame, size_t toFrame, const NoteSegmenterSettings& settings = {});

// Vibrato-free pitch line (MIDI, 0 where unvoiced): centred running median of the voiced frames within
// +-smoothSeconds / 2 (narrower near the ends of each voiced run, symmetric), each voiced run on its own.
std::vector<double> smoothedPitchMidi (const PitchTrack& track, double smoothSeconds);

// Splits a pYIN pitch track into notes using held pitch levels and level onsets.
class NoteSegmenter
{
public:
    explicit NoteSegmenter (NoteSegmenterSettings settingsIn = {}) : settings (settingsIn) {}

    std::vector<SungNote> segment (const PitchTrack& track) const;

private:
    NoteSegmenterSettings settings;
};

} // namespace shizuku
