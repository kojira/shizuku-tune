// Tests for reference melody candidates (ReferenceSelection.h) and MIDI parts / confidence (loadMidi):
//  - a two-track MIDI file with "shizuku:velocity=confidence" loads as two named parts with confidences,
//  - per phrase the matching candidate is chosen (including a whole-octave difference),
//  - a note sung 4 semitones off its phrase's reference is pulled there only when the reference note's
//    confidence and the neighbouring phrases' match are high; otherwise it keeps the scale target, marked unsure,
//  - call and response: a phrase that echoes the reference two beats later uses the reference shifted by two
//    beats (not the notes sounding at the same time), and an unshifted match is not replaced by a shift,
//  - alignment: a note sliding from one reference note towards the next without reaching it is divided at the
//    reference boundary into one part per reference note, pieces of one reference note are joined, and the reach
//    (time within 50 cents of the target) tells a held note from one that never gets there.
// Exits non-zero when any check fails.

#include "../src/analysis/ReferenceMelody.h"
#include "../src/analysis/MusicTheory.h"
#include "../src/analysis/ReferenceSelection.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using namespace shizuku;

namespace
{
int failures = 0;

void check (bool condition, const char* what)
{
    std::printf ("  %s: %s\n", condition ? "ok  " : "FAIL", what);
    if (! condition)
        ++failures;
}

std::string varLen (uint32_t value)
{
    std::string out (1, (char) (value & 0x7f));
    while ((value >>= 7) != 0)
        out.insert (out.begin(), (char) ((value & 0x7f) | 0x80));
    return out;
}

std::string be (uint32_t value, int bytes)
{
    std::string out;
    for (int i = bytes - 1; i >= 0; --i)
        out += (char) ((value >> (8 * i)) & 0xff);
    return out;
}

struct MidiNote
{
    double start, end;
    int key, velocity;
};

// One MTrk chunk at 120 BPM / 480 ticks per beat (960 ticks per second).
std::string track (const std::string& name, const std::vector<MidiNote>& notes, bool confidenceMarker)
{
    std::string events = varLen (0) + "\xff\x03" + varLen ((uint32_t) name.size()) + name;
    if (confidenceMarker)
    {
        const std::string marker = "shizuku:velocity=confidence";
        events += varLen (0) + "\xff\x01" + varLen ((uint32_t) marker.size()) + marker;
    }
    uint32_t last = 0;
    for (const auto& note : notes)
    {
        const auto on = (uint32_t) std::lround (note.start * 960.0), off = (uint32_t) std::lround (note.end * 960.0);
        events += varLen (on - last) + std::string ({ (char) 0x90, (char) note.key, (char) note.velocity });
        events += varLen (off - on) + std::string ({ (char) 0x80, (char) note.key, (char) 0 });
        last = off;
    }
    events += varLen (0) + "\xff\x2f" + std::string (1, '\0');
    return "MTrk" + be ((uint32_t) events.size(), 4) + events;
}

NoteWithKey sung (double start, double end, double centreMidi, int scaleTarget)
{
    NoteWithKey entry;
    entry.note.startSeconds = start;
    entry.note.endSeconds = end;
    entry.note.centreMidi = centreMidi;
    entry.note.nearestMidi = (int) std::lround (centreMidi);
    entry.nearestScaleMidi = scaleTarget;
    return entry;
}
} // namespace

int main()
{
    std::printf ("MIDI parts and confidence:\n");
    const std::string path = "/tmp/shizuku-reference-selection-test.mid";
    {
        // Part "lead (A)": 0-4 s, one note per second; part "lead (B)": 10-14 s. Velocity = confidence.
        std::vector<MidiNote> a { { 0.0, 1.0, 60, 127 }, { 1.0, 2.0, 62, 127 }, { 2.0, 3.0, 64, 121 }, { 3.0, 4.0, 65, 127 } };
        std::vector<MidiNote> b { { 10.0, 11.0, 67, 127 }, { 11.0, 12.0, 69, 25 }, { 12.0, 13.0, 71, 127 }, { 13.0, 14.0, 72, 127 } };
        std::ofstream out (path, std::ios::binary);
        out << "MThd" << be (6, 4) << be (1, 2) << be (2, 2) << be (480, 2) << track ("lead (A)", a, true) << track ("lead (B)", b, true);
    }
    const auto lead = loadMidi (path);
    check (lead.partNames.size() == 2 && lead.partNames[0] == "lead (A)" && lead.partNames[1] == "lead (B)", "two parts named from the tracks");
    check (lead.notes.size() == 8 && lead.notes[4].part == 1, "8 notes, the later ones in part 1");
    check (std::abs (lead.notes[2].confidence - 121.0 / 127.0) < 1.0e-6 && std::abs (lead.notes[5].confidence - 25.0 / 127.0) < 1.0e-6,
           "velocity read as confidence");
    check (std::abs (lead.notes[1].startSeconds - 1.0) < 1.0e-6, "120 BPM timing");

    // A guide that only covers 20-24 s.
    ReferenceMelody guide;
    guide.sourceName = "guide";
    guide.partNames = { "all" };
    for (int i = 0; i < 4; ++i)
        guide.notes.push_back ({ 20.0 + i, 21.0 + i, 64 + i, 1.0, 0 });

    std::vector<ReferenceCandidate> candidates (2);
    candidates[0].name = "lead";
    candidates[0].melody = lead;
    candidates[1].name = "guide";
    candidates[1].melody = guide;

    // Phrase 1 (0-4 s): the lead one octave up, 30 cents flat; the 3rd note (lead E4, confidence 0.95) sung
    // 4 semitones low. Phrase 2 (10-14 s): the lead, 20 cents sharp; the 2nd note (lead A4, confidence 0.2)
    // sung 4 semitones low. Phrase 3 (20-24 s): the guide.
    std::vector<NoteWithKey> notes {
        sung (0.05, 0.95, 71.7, 72), sung (1.05, 1.95, 73.7, 74), sung (2.05, 2.95, 72.0, 72), sung (3.05, 3.95, 76.7, 77),
        sung (10.05, 10.95, 67.2, 67), sung (11.05, 11.95, 65.0, 65), sung (12.05, 12.95, 71.2, 71), sung (13.05, 13.95, 72.2, 72),
        sung (20.05, 20.95, 64.1, 64), sung (21.05, 21.95, 64.9, 65), sung (22.05, 22.95, 66.1, 66), sung (23.05, 23.95, 66.8, 67),
    };

    std::printf ("Selection per phrase:\n");
    const auto selection = selectReferences (notes, candidates);
    std::printf ("%s", describeSelection (selection, candidates).c_str());
    check (selection.segments.size() == 3, "three phrases");
    check (selection.segments[0].candidate == 0 && selection.segments[0].octave == 1 && selection.segments[0].part == 0,
           "phrase 1: lead, part A, one octave up");
    check (selection.segments[1].candidate == 0 && selection.segments[1].octave == 0 && selection.segments[1].part == 1,
           "phrase 2: lead, part B, same octave");
    check (selection.segments[2].candidate == 1, "phrase 3: guide");

    std::printf ("Targets:\n");
    auto applied = notes;
    const auto stats = applySelectedReferences (applied, candidates, selection);
    check (applied[0].nearestScaleMidi == 72 && applied[0].hasReferenceTarget && ! applied[0].largeReferenceShift, "near note -> lead C5");
    check (applied[2].nearestScaleMidi == 76 && applied[2].largeReferenceShift, "4 semitones off, confident reference -> pulled to E5");
    check (applied[5].nearestScaleMidi == 65 && ! applied[5].hasReferenceTarget && applied[5].referenceUnsure,
           "4 semitones off, unsure reference (0.2) -> scale target kept, marked");
    check (applied[9].nearestScaleMidi == 65 && applied[9].referenceCandidate == 1, "guide phrase -> guide note");
    check (stats.largeShifts == 1 && stats.unsure == 1, "one large shift, one unsure note");

    std::printf ("Distance limit by confidence (estimated references):\n");
    {
        ReferenceSelectionSettings limits;
        check (std::abs (limits.nearLimitForConfidence (0.1) - 1.0) < 1.0e-9 && std::abs (limits.nearLimitForConfidence (0.75) - 1.0) < 1.0e-9,
               "confidence <= 0.75: at most 1 semitone");
        check (std::abs (limits.nearLimitForConfidence (0.825) - 1.75) < 1.0e-9, "0.825: half way to 2.5 semitones");
        check (std::abs (limits.nearLimitForConfidence (0.95) - 2.5) < 1.0e-9 && std::abs (limits.nearLimitForConfidence (1.0) - 2.5) < 1.0e-9,
               "confidence >= 0.9 and written scores: 2.5 semitones");
        // The lead's 4th note (F4, confidence 1) sung 1.9 semitones low: still moved. The same note at confidence
        // 0.5 (an uncertain estimate): the scale target near the singing is kept and the note is marked unsure.
        auto unsureCandidates = candidates;
        auto sharp = notes;
        sharp[3].note.centreMidi = 75.1;
        auto confident = sharp;
        applySelectedReferences (confident, unsureCandidates, selection);
        check (confident[3].hasReferenceTarget && confident[3].nearestScaleMidi == 77, "1.9 semitones, confidence 1 -> lead F5");
        unsureCandidates[0].melody.notes[3].confidence = 0.5;
        auto unsure = sharp;
        unsure[3].nearestScaleMidi = 76;
        applySelectedReferences (unsure, unsureCandidates, selection);
        check (! unsure[3].hasReferenceTarget && unsure[3].referenceUnsure && unsure[3].nearestScaleMidi == 76,
               "1.9 semitones, confidence 0.5 -> scale target kept, marked unsure");
    }

    std::printf ("Forced choice and none:\n");
    const auto forced = selectReferences (notes, candidates, {}, 1);
    check (forced.segments[0].candidate < 0 && forced.segments[2].candidate == 1, "forced guide: only where the guide has notes");

    const auto errors = referencePitchForErrors (notes, candidates, selection);
    check (std::abs (errors[0] - 72.0) < 1.0e-9 && std::abs (errors[2] - 76.0) < 1.0e-9 && std::abs (errors[5] - 69.0) < 1.0e-9,
           "error reference at the phrase's octave");

    std::printf ("Call and response (beat shift):\n");
    {
        // 120 BPM (beat 0.5 s). Lead: a held D5 at 30-32 s after a rising B4 C5 C#5 at 29-30 s ... each 0.25 s,
        // then the same again at 34 s. The echo sings B4 C5 C#5 D5 one second (two beats) after the lead, i.e.
        // while the lead holds D5.
        ReferenceMelody echoLead;
        echoLead.sourceName = "lead";
        echoLead.partNames = { "all" };
        for (double base : { 29.25, 34.25 })
        {
            echoLead.notes.push_back ({ base, base + 0.25, 71, 0.95, 0 });
            echoLead.notes.push_back ({ base + 0.25, base + 0.5, 72, 0.95, 0 });
            echoLead.notes.push_back ({ base + 0.5, base + 0.75, 73, 0.95, 0 });
            echoLead.notes.push_back ({ base + 0.75, base + 2.75, 74, 0.95, 0 });
        }
        std::vector<ReferenceCandidate> echoCandidates (1);
        echoCandidates[0].name = "lead";
        echoCandidates[0].melody = echoLead;

        // Echo at 30.25-31.5 s (two beats late), 30 cents flat; the second phrase (35.25 s) on time with the lead's D5.
        std::vector<NoteWithKey> echo {
            sung (30.25, 30.48, 70.7, 71), sung (30.5, 30.73, 71.7, 72), sung (30.75, 30.98, 72.7, 72), sung (31.0, 31.5, 73.7, 74),
            sung (35.25, 35.9, 74.2, 74), sung (35.95, 36.6, 73.8, 74), sung (36.65, 37.0, 74.1, 74),
        };
        ReferenceSelectionSettings settings;
        settings.beatSeconds = 0.5;
        const auto echoSelection = selectReferences (echo, echoCandidates, settings);
        std::printf ("%s", describeSelection (echoSelection, echoCandidates).c_str());
        check (echoSelection.segments.size() == 2, "two phrases");
        check (echoSelection.segments[0].candidate == 0 && echoSelection.segments[0].chosenShiftBeats() == 2, "echo phrase: lead two beats later");
        check (echoSelection.segments[1].chosenShiftBeats() == 0, "on-time phrase: no shift");
        auto echoApplied = echo;
        applySelectedReferences (echoApplied, echoCandidates, echoSelection, settings);
        check (echoApplied[0].nearestScaleMidi == 71 && echoApplied[2].nearestScaleMidi == 73 && echoApplied[0].referenceShiftBeats == 2,
               "echo targets B4 / C#5 (not the held D5)");

        const auto withoutBeat = selectReferences (echo, echoCandidates);
        auto unshifted = echo;
        applySelectedReferences (unshifted, echoCandidates, withoutBeat);
        check (withoutBeat.segments[0].chosenShiftBeats() == 0 && unshifted[0].nearestScaleMidi != 71,
               "without a beat length the echo is matched at the same time (as before)");
    }

    std::printf ("Alignment to the reference notes and reach:\n");
    {
        // Reference: G4 1.0-1.4 s, A4 1.4-1.8 s, then B4 2.0-2.4 s. Sung: G4 (vibrato) from 1.0 s, then sliding up
        // from 1.35 s to 68.4 (60 cents short of A4) without settling until 1.8 s - one note for the segmenter;
        // then B4 in two pieces (a 20 ms gap at 2.2 s, as a breath or a pYIN dropout).
        ReferenceMelody melody;
        melody.sourceName = "ref";
        melody.partNames = { "all" };
        melody.notes = { { 1.0, 1.4, 67, 1.0, 0 }, { 1.4, 1.8, 69, 1.0, 0 }, { 2.0, 2.4, 71, 1.0, 0 } };
        SongAnalysis analysis;
        analysis.hasVocal = true;
        analysis.pitch.hopSeconds = 0.0058;
        for (int f = 0; f * analysis.pitch.hopSeconds < 2.6; ++f)
        {
            const double t = f * analysis.pitch.hopSeconds;
            double midi = 0.0;
            if (t >= 1.0 && t < 1.8)
                midi = (t < 1.35 ? 67.0 : 67.0 + 1.4 * std::min (1.0, (t - 1.35) / 0.45)) + 0.25 * std::sin (2.0 * M_PI * 5.8 * t);
            else if (t >= 2.0 && t < 2.4 && std::abs (t - 2.21) > 0.01)
                midi = 70.8 + 0.25 * std::sin (2.0 * M_PI * 5.8 * t);
            analysis.pitch.frames.push_back ({ t, midi > 0.0 ? midiToFrequency (midi) : 0.0, 0.9, -20.0, midi > 0.0 });
        }
        for (const auto& note : NoteSegmenter().segment (analysis.pitch))
        {
            NoteWithKey entry;
            entry.note = note;
            entry.nearestScaleMidi = note.nearestMidi;
            analysis.notes.push_back (entry);
        }
        std::printf ("  segmenter: %zu notes:", analysis.notes.size());
        for (const auto& entry : analysis.notes)
            std::printf (" [%.3f-%.3f %.2f]", entry.note.startSeconds, entry.note.endSeconds, entry.note.centreMidi);
        std::printf ("\n");
        std::vector<ReferenceCandidate> refCandidates (1);
        refCandidates[0].name = "ref";
        refCandidates[0].melody = melody;
        auto refSelection = selectReferences (analysis.notes, refCandidates);
        const auto alignment = alignNotesToReferences (analysis, refCandidates, refSelection);
        applySelectedReferences (analysis.notes, refCandidates, refSelection);
        measureReach (analysis.notes, analysis.pitch);
        std::printf ("  aligned: %zu notes -> %zu parts:", alignment.notesBefore, alignment.partsAfter);
        for (const auto& entry : analysis.notes)
            std::printf (" [%.3f-%.3f ref %ld target %d reach %.0f%% curve %d]", entry.note.startSeconds, entry.note.endSeconds,
                         entry.alignedReferenceNote, entry.nearestScaleMidi, entry.reachFraction * 100.0, (int) entry.referenceCurve);
        std::printf ("\n");
        const auto& parts = analysis.notes;
        check (parts.size() == 3, "three parts: G4, the slide towards A4, B4 (its two pieces joined)");
        if (parts.size() == 3)
        {
            check (parts[0].alignedReferenceNote == 0 && parts[1].alignedReferenceNote == 1 && parts[2].alignedReferenceNote == 2,
                   "each part aimed at its own reference note");
            check (parts[0].note.endSeconds >= 1.33 && parts[0].note.endSeconds <= 1.65,
                   "the slide is split from G4 between the reference boundary / start of the slide and its pitch midpoint (1.33-1.65 s)");
            check (parts[0].nearestScaleMidi == 67 && parts[1].nearestScaleMidi == 69 && parts[2].nearestScaleMidi == 71, "targets G4 / A4 / B4");
            check (parts[0].reachFraction > 0.9 && parts[1].reachFraction < 0.5, "reach: G4 held at the target, the slide mostly short of A4");
            check (parts[0].referenceCurve && parts[1].referenceCurve, "aligned parts are corrected along the target pitch curve");
        }
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
