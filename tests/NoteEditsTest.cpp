// Hand edits of single notes (NoteEdits.h, planCorrection overrides, renderEditedSpans):
// - an edited note ends at its target (fractional MIDI, 1-cent precision), even where the automatic rules would
//   not correct it (below the threshold); a "keep" note stays at its sung pitch;
// - every other note keeps its automatic decision, and outside the re-rendered spans the output is bit-identical
//   to the automatic render;
// - edits find their note when the note boundaries move a little; the JSON round trip keeps whole cents.
// Exits non-zero when any check fails.

#include "../src/analysis/SongAnalyzer.h"
#include "../src/correction/NoteEdits.h"
#include "../src/correction/PitchCorrection.h"
#include "../tools/SyntheticSignals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace shizuku;

namespace
{
constexpr double sampleRate = 44100.0;
int failures = 0;

void check (bool condition, const char* what)
{
    std::printf ("  %s: %s\n", condition ? "ok  " : "FAIL", what);
    if (! condition)
        ++failures;
}

double medianPitchBetween (const PitchTrack& track, double from, double to)
{
    std::vector<double> values;
    for (const auto& frame : track.frames)
        if (frame.voiced && frame.timeSeconds >= from && frame.timeSeconds < to)
            values.push_back (frequencyToMidi (frame.frequencyHz));
    if (values.empty())
        return 0.0;
    std::nth_element (values.begin(), values.begin() + (long) values.size() / 2, values.end());
    return values[values.size() / 2];
}

double centreOf (const PitchTrack& track, const SungNote& note)
{
    const double length = note.endSeconds - note.startSeconds;
    return medianPitchBetween (track, note.startSeconds + 0.2 * length, note.endSeconds - 0.2 * length);
}

void jsonAndMatching()
{
    std::printf ("JSON and note matching:\n");
    const auto document = parseNoteEdits (R"({"format":"shizuku-note-edits","formatVersion":1,"file":"\u304b.wav","baseVersion":"v1",
        "edits":[{"start":1.0,"end":1.5,"mode":"target","targetMidi":62.37499,"created":"t","version":"v1"},
                 {"start":2.0,"end":2.4,"mode":"keep"},{"start":3.0,"end":3.2,"mode":"auto"}],"history":[1,2]})");
    check (document.file == "\xe3\x81\x8b.wav" && document.baseVersion == "v1" && document.edits.size() == 3, "document fields (\\u escapes decoded)");
    check (std::abs (document.edits[0].targetMidi - 62.37) < 1.0e-12, "target rounded to whole cents (62.37499 -> 62.37)");
    const auto again = parseNoteEdits (toJson (document));
    check (again.edits.size() == 3 && again.edits[0].targetMidi == document.edits[0].targetMidi && again.edits[1].mode == NoteEditMode::keep
               && again.edits[2].mode == NoteEditMode::automatic && again.file == document.file,
           "JSON round trip keeps the edits exactly");
    bool threw = false;
    try
    {
        parseNoteEdits (R"({"edits":[{"start":1,"mode":"target"}]})");
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    check (threw, "a target edit without targetMidi is rejected");

    std::vector<NoteWithKey> notes (4);
    const double spans[4][2] = { { 0.50, 0.90 }, { 0.95, 1.40 }, { 2.00, 2.15 }, { 2.16, 2.40 } };
    for (size_t i = 0; i < 4; ++i)
    {
        notes[i].note.startSeconds = spans[i][0];
        notes[i].note.endSeconds = spans[i][1];
    }
    std::vector<NoteEdit> edits (4);
    edits[0] = { 0.53, 0.94, NoteEditMode::target, 64.0, "", "" };  // boundaries moved by 30-40 ms -> note 0
    edits[1] = { 1.45, 1.50, NoteEditMode::keep, 0.0, "", "" };      // no overlap, 50 ms after note 1 -> note 1
    edits[2] = { 2.00, 2.40, NoteEditMode::target, 65.5, "", "" };   // one edited note now segmented as two -> both
    edits[3] = { 5.00, 5.30, NoteEditMode::keep, 0.0, "", "" };      // nothing near
    std::vector<size_t> unmatched;
    const auto overrides = resolveNoteEdits (notes, edits, {}, &unmatched);
    check (overrides[0].mode == NoteEditMode::target && overrides[0].targetMidi == 64.0, "edit with moved boundaries finds its note");
    check (overrides[1].mode == NoteEditMode::keep, "edit next to a note (no overlap, < 150 ms) finds it");
    check (overrides[2].mode == NoteEditMode::target && overrides[3].mode == NoteEditMode::target, "a note split in two gets the edit on both parts");
    check (unmatched.size() == 1 && unmatched[0] == 3, "an edit with no note nearby is reported unmatched");
    const auto later = resolveNoteEdits (notes, { edits[0], { 0.5, 0.9, NoteEditMode::automatic, 0.0, "", "" } });
    check (later[0].mode == NoteEditMode::automatic, "a later edit of the same note replaces the earlier one");
}

void editedRender()
{
    std::printf ("Edited render (synthetic A minor melody):\n");
    const MusicalKey key { 9, KeyMode::minor };
    const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
    const auto melody = synth::detunedMelody (key, 60, 0.5);
    const auto vocal = synth::renderVocal (melody, synth::VoiceTimbre::voiceLike, sampleRate, accompaniment.size() / sampleRate);
    const auto analysis = analyseSong (vocal, sampleRate, accompaniment, sampleRate);
    const CorrectionSettings settings;
    const auto automaticCurve = planCorrection (analysis, settings);
    const double hint = medianVoicedFrequency (analysis.pitch);
    const auto automatic = renderCorrection ({ vocal }, sampleRate, automaticCurve, 0.0, hint);

    // Pick: a corrected long note to move up a semitone and 37 cents off, a kept note (|deviation| < threshold)
    // to force onto a target 30 cents above its scale note, and a corrected note to keep as sung.
    long moved = -1, forced = -1, kept = -1;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const auto& note = analysis.notes[i].note;
        const double length = note.endSeconds - note.startSeconds;
        if (length < 0.3)
            continue;
        const auto& decision = automaticCurve.notes[i];
        if (moved < 0 && decision.corrected && ! decision.follows)
            moved = (long) i;
        else if (forced < 0 && ! decision.corrected)
            forced = (long) i;
        else if (kept < 0 && decision.corrected && ! decision.follows && (long) i > moved + 1)
            kept = (long) i;
    }
    check (moved >= 0 && forced >= 0 && kept >= 0, "test melody has a corrected, a kept and another corrected long note");
    if (moved < 0 || forced < 0 || kept < 0)
        return;
    const auto& movedNote = analysis.notes[(size_t) moved].note;
    const auto& forcedNote = analysis.notes[(size_t) forced].note;
    const auto& keptNote = analysis.notes[(size_t) kept].note;
    const double movedTarget = automaticCurve.notes[(size_t) moved].targetMidi + 1.37;
    const double forcedTarget = analysis.notes[(size_t) forced].nearestScaleMidi + 0.30;
    NoteEditDocument document;
    document.edits = { { movedNote.startSeconds + 0.02, movedNote.endSeconds - 0.01, NoteEditMode::target, movedTarget, "", "" },
                       { forcedNote.startSeconds, forcedNote.endSeconds, NoteEditMode::target, forcedTarget, "", "" },
                       { keptNote.startSeconds, keptNote.endSeconds, NoteEditMode::keep, 0.0, "", "" } };
    const auto edits = parseNoteEdits (toJson (document)).edits;
    const auto overrides = resolveNoteEdits (analysis.notes, edits);
    const auto editedCurve = planCorrection (analysis, settings, overrides);
    const auto edited = renderEditedSpans ({ vocal }, sampleRate, automaticCurve, automatic, editedCurve, 0.0, hint);

    size_t sameDecisions = 0, others = 0;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        if ((long) i == moved || (long) i == forced || (long) i == kept || editedCurve.notes[i].follows)
            continue;
        ++others;
        sameDecisions += editedCurve.notes[i].shiftSemitones == automaticCurve.notes[i].shiftSemitones
                         && editedCurve.notes[i].corrected == automaticCurve.notes[i].corrected ? 1 : 0;
    }
    check (others > 0 && sameDecisions == others, "the other (non-passing) notes keep their automatic decisions");
    check (editedCurve.notes[(size_t) moved].edited && editedCurve.notes[(size_t) kept].edited && ! editedCurve.notes[(size_t) kept].corrected,
           "edited notes are marked; the keep note is not corrected");

    // Outside the re-rendered spans: bit-identical to the automatic render.
    const auto inSpan = [&] (long sample)
    {
        const double t = (double) sample / sampleRate;
        return std::any_of (edited.spans.begin(), edited.spans.end(), [&] (const TimeSpan& s) { return t >= s.startSeconds - 1.0 / sampleRate && t <= s.endSeconds + 1.0 / sampleRate; });
    };
    size_t differentOutside = 0;
    for (long s = 0; s < (long) vocal.size(); ++s)
        if (! inSpan (s) && edited.output[0][(size_t) s] != automatic[0][(size_t) s])
            ++differentOutside;
    double spanSeconds = 0.0;
    for (const auto& span : edited.spans)
        spanSeconds += span.endSeconds - span.startSeconds;
    std::printf ("  %zu re-rendered spans, %.2f s of %.2f s\n", edited.spans.size(), spanSeconds, vocal.size() / sampleRate);
    check (! edited.spans.empty() && differentOutside == 0, "outside the re-rendered spans the output equals the automatic render exactly");
    check (spanSeconds < 0.5 * (double) vocal.size() / sampleRate, "only the edited notes' surroundings are re-rendered");

    // Edited notes land where they were told to; untouched notes measure as before.
    const auto afterEdit = PitchTracker().analyse (edited.output[0], sampleRate);
    const auto afterAutomatic = PitchTracker().analyse (automatic[0], sampleRate);
    const double movedError = std::abs (centreOf (afterEdit, movedNote) - movedTarget) * 100.0;
    const double forcedError = std::abs (centreOf (afterEdit, forcedNote) - forcedTarget) * 100.0;
    const double keptError = std::abs (centreOf (afterEdit, keptNote) - centreOf (analysis.pitch, keptNote)) * 100.0;
    std::printf ("  moved note: target %.2f, measured error %.1fc; forced (below threshold): target %.2f, error %.1fc; keep: %.1fc from sung\n",
                 movedTarget, movedError, forcedTarget, forcedError, keptError);
    check (movedError <= 8.0, "edited note (+1 semitone 37 cents) measures within 8 cents of its target");
    check (forcedError <= 8.0, "edit overrides the threshold: a note the automatic rules kept moves to its target");
    check (keptError <= 3.0, "a keep edit leaves the note at its sung pitch");
    double maxOtherChange = 0.0;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
        if ((long) i != moved && (long) i != forced && (long) i != kept && ! editedCurve.notes[i].follows)
            maxOtherChange = std::max (maxOtherChange, std::abs (centreOf (afterEdit, analysis.notes[i].note) - centreOf (afterAutomatic, analysis.notes[i].note)) * 100.0);
    std::printf ("  other notes: max change of the measured centre %.2fc\n", maxOtherChange);
    check (maxOtherChange <= 1.0, "the other notes measure the same as in the automatic render (within 1 cent)");

    // Removing every edit returns the automatic render bit for bit; a repeated render reuses unchanged spans.
    const auto none = renderEditedSpans ({ vocal }, sampleRate, automaticCurve, automatic, planCorrection (analysis, settings, {}), 0.0, hint);
    check (none.spans.empty() && none.output == automatic, "no edits: output is the automatic render exactly");
    const auto repeated = renderEditedSpans ({ vocal }, sampleRate, automaticCurve, automatic, editedCurve, 0.0, hint, &edited, &editedCurve);
    check (repeated.renderedSpans.empty() && repeated.output == edited.output, "same edits again: nothing re-rendered, same output");
    auto oneLess = edits;
    oneLess.pop_back();
    const auto oneLessCurve = planCorrection (analysis, settings, resolveNoteEdits (analysis.notes, oneLess));
    const auto undone = renderEditedSpans ({ vocal }, sampleRate, automaticCurve, automatic, oneLessCurve, 0.0, hint, &edited, &editedCurve);
    const auto fresh = renderEditedSpans ({ vocal }, sampleRate, automaticCurve, automatic, oneLessCurve, 0.0, hint);
    check (undone.renderedSpans.size() < undone.spans.size() || undone.spans.size() <= 1, "undoing one edit re-renders only its span");
    check (undone.output == fresh.output, "the result does not depend on the order of edits (incremental = fresh)");
}
// Splits and merges ("structure"): JSON round trip (old format without "structure" still read), splitting a note
// at a time and joining notes, unmatched structure edits, and split parts corrected separately.
void splitAndMerge()
{
    std::printf ("Splits and merges:\n");
    const auto old = parseNoteEdits (R"({"format":"shizuku-note-edits","formatVersion":1,"edits":[{"start":1.0,"end":1.5,"mode":"keep"}]})");
    check (old.structure.empty() && old.edits.size() == 1, "formatVersion 1 (no structure) is read");
    NoteEditDocument document;
    document.structure = { { NoteStructureEdit::Op::split, 0.75, 0.0, 0.0, "t", "v2" }, { NoteStructureEdit::Op::merge, 0.0, 1.0, 1.7, "", "" } };
    const auto again = parseNoteEdits (toJson (document));
    check (again.structure.size() == 2 && again.structure[0].op == NoteStructureEdit::Op::split && again.structure[0].timeSeconds == 0.75
               && again.structure[1].op == NoteStructureEdit::Op::merge && again.structure[1].startSeconds == 1.0 && again.structure[1].endSeconds == 1.7
               && again.structure[0].version == "v2",
           "structure JSON round trip");

    // Pitch track: 0.5-1.0 s at 69.4 (one note), 1.1-1.3 s at 71.3 and 1.3-1.5 s at 71.6 (two notes, connected).
    SongAnalysis analysis;
    analysis.hasVocal = true;
    analysis.pitch.hopSeconds = 0.005;
    for (int f = 0; f * 0.005 < 1.7; ++f)
    {
        const double t = f * 0.005;
        const double midi = t >= 0.5 && t < 1.0 ? (t < 0.75 ? 69.4 : 70.3) : t >= 1.1 && t < 1.5 ? (t < 1.3 ? 71.3 : 71.6) : 0.0;
        analysis.pitch.frames.push_back ({ t, midi > 0.0 ? midiToFrequency (midi) : 0.0, 0.9, -20.0, midi > 0.0 });
    }
    const auto noteOf = [&] (double from, double to, double centre)
    {
        NoteWithKey entry;
        entry.note.startSeconds = from;
        entry.note.endSeconds = to;
        entry.note.centreMidi = centre;
        entry.nearestScaleMidi = (int) std::lround (centre);
        return entry;
    };
    analysis.notes = { noteOf (0.5, 1.0, 69.4), noteOf (1.1, 1.3, 71.3), noteOf (1.3, 1.5, 71.6) };
    std::vector<size_t> unmatched;
    const std::vector<NoteStructureEdit> structure = { { NoteStructureEdit::Op::split, 0.75, 0.0, 0.0, "", "" },
                                                       { NoteStructureEdit::Op::merge, 0.0, 1.15, 1.45, "", "" },
                                                       { NoteStructureEdit::Op::split, 1.05, 0.0, 0.0, "", "" } }; // in a gap
    const auto notes = applyNoteStructure (analysis, analysis.notes, structure, {}, &unmatched);
    for (const auto& entry : notes)
        std::printf ("    %.3f-%.3f centre %.2f aligned %ld\n", entry.note.startSeconds, entry.note.endSeconds, entry.note.centreMidi, entry.alignedReferenceNote);
    check (notes.size() == 3, "split one note, merged two: three notes");
    if (notes.size() == 3)
    {
        check (std::abs (notes[0].note.endSeconds - 0.75) < 0.006 && std::abs (notes[1].note.startSeconds - 0.75) < 0.006,
               "split at the given time");
        check (std::abs (notes[0].note.centreMidi - 69.4) < 0.01 && std::abs (notes[1].note.centreMidi - 70.3) < 0.01,
               "each half measured on its own (69.4 / 70.3)");
        check (std::abs (notes[2].note.startSeconds - 1.1) < 0.006 && std::abs (notes[2].note.endSeconds - 1.5) < 0.006,
               "merged note spans both");
        check (notes[0].alignedReferenceNote == -2 && notes[2].alignedReferenceNote == -2, "new notes are marked for realignment by time");
    }
    check (unmatched.size() == 1 && unmatched[0] == 2, "a split in a gap is reported unmatched");
    // Applying the same structure to the result again changes nothing (a split at an existing boundary, a merge of one note).
    std::vector<size_t> again2;
    const auto twice = applyNoteStructure (analysis, notes, { structure[0], structure[1] }, {}, &again2);
    check (twice.size() == notes.size() && again2.empty(), "splits / merges already in place are no-ops (not unmatched)");

    // Split parts are corrected separately: targets 69 and 70 -> shifts -0.4 / -0.3.
    analysis.notes = notes;
    analysis.notes[0].nearestScaleMidi = 69;
    analysis.notes[1].nearestScaleMidi = 70;
    const auto curve = planCorrection (analysis, CorrectionSettings {});
    check (std::abs (curve.notes[0].shiftSemitones + 0.4) < 0.02 && std::abs (curve.notes[1].shiftSemitones + 0.3) < 0.02,
           "the two halves of a split note get their own shifts");
    // An edit made on the merged note finds it after the merge; an edit on a half finds that half.
    const auto overrides = resolveNoteEdits (analysis.notes, { { 1.1, 1.5, NoteEditMode::keep, 0.0, "", "" }, { 0.76, 1.0, NoteEditMode::target, 70.0, "", "" } });
    check (overrides[2].mode == NoteEditMode::keep && overrides[1].mode == NoteEditMode::target && overrides[0].mode == NoteEditMode::automatic,
           "edits on split / merged notes find them by time");
}
} // namespace

int main()
{
    jsonAndMatching();
    splitAndMerge();
    editedRender();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
