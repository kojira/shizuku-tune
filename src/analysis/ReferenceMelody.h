#pragma once

#include "SongAnalyzer.h"

#include <string>
#include <vector>

namespace shizuku
{

// The intended melody of a part (e.g. a harmony line), in seconds from the start of the song.
struct ReferenceNote
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    int midi = 0;
    double confidence = 1.0; // 0..1: how sure the reference is of this note (written scores: 1)
    int part = 0;            // index into ReferenceMelody::partNames
};

struct ReferenceMelody
{
    std::string sourceName;
    std::vector<std::string> partNames; // e.g. MIDI track names ("lead (singer A)"); at least one entry
    std::vector<ReferenceNote> notes;   // sorted by start time

    std::string partName (int part) const;
};

// Synthesizer V project (.svp, JSON; trailing NULs allowed). Uses every track's main group, the project's
// tempo map, and each note's pitch (+ detune is ignored: it is a rendering nuance, not the written note).
ReferenceMelody loadSvp (const std::string& path);

// Standard MIDI file (format 0 or 1), all channels, with the file's tempo map. Each track with notes is a
// part (named by its track-name event). A text event "shizuku:velocity=confidence" in a track makes note-on
// velocity the note's confidence (velocity / 127), e.g. for a melody estimated from a recording.
ReferenceMelody loadMidi (const std::string& path);

// Notes of a sung/synthesised guide recording (pYIN + note segmentation), rounded to the nearest semitone.
ReferenceMelody referenceFromAudio (const std::vector<float>& monoSamples, double sampleRate, const std::string& name);

// .svp / .mid / .midi / .wav by file extension. Throws std::runtime_error on failure.
ReferenceMelody loadReference (const std::string& path);

struct ReferenceAlignment
{
    double offsetSeconds = 0.0;   // vocal time = reference time + offsetSeconds
    double matchedFraction = 0.0; // fraction of the vocal's note time whose pitch class agrees with the reference
    size_t matchedNotes = 0;
    size_t consideredNotes = 0;
};

// Finds the time offset (vocal time = reference time + offset) within +-maxOffsetSeconds that makes the most
// sung note time agree in pitch class (within half a semitone, any octave) with the reference note sounding
// at the same moment.
ReferenceAlignment alignReference (const std::vector<NoteWithKey>& notes, const ReferenceMelody& reference,
                                   double maxOffsetSeconds = 10.0);

struct ReferenceTargetSettings
{
    double offsetSeconds = 0.0;       // vocal time = reference time + offset
    double maxDistanceSemitones = 2.5; // farther than this from the reference (nearest octave): keep the scale target
};

// For each sung note, the reference note sounding at its midpoint becomes its target, transposed by whole
// octaves to the octave closest to what was sung. Returns the number of notes that got a reference target.
size_t applyReferenceTargets (std::vector<NoteWithKey>& notes, const ReferenceMelody& reference, const ReferenceTargetSettings& settings);

} // namespace shizuku
