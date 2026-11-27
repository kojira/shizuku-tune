#include "CorrectionSetup.h"

#include "../src/analysis/MusicTheory.h"
#include "../src/analysis/ReferenceMelody.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shizuku::tools
{

namespace
{
void appendf (std::string& out, const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start (args, format);
    std::vsnprintf (buffer, sizeof (buffer), format, args);
    va_end (args);
    out += buffer;
}
} // namespace

ReferenceArgument parseReferenceArgument (const std::string& text)
{
    ReferenceArgument argument;
    argument.path = text;
    const auto at = argument.path.find_last_of ('@');
    if (at != std::string::npos)
    {
        argument.offset = std::atof (argument.path.c_str() + at + 1);
        argument.offsetGiven = true;
        argument.path.resize (at);
    }
    return argument;
}

PreparedCorrection prepareCorrection (const std::string& vocalPath, const std::string& accompanimentPath,
                                      const std::vector<ReferenceArgument>& references, int choose, double bpmOverride,
                                      bool describePhrases)
{
    PreparedCorrection prepared;
    prepared.vocal = readWav (vocalPath);
    std::vector<float> accompaniment;
    double accompanimentRate = 0.0;
    if (accompanimentPath != "-")
    {
        const auto wav = readWav (accompanimentPath);
        accompaniment = wav.mixToMono();
        accompanimentRate = wav.sampleRate;
    }
    auto& analysis = prepared.analysis;
    auto& log = prepared.log;
    analysis = analyseSong (prepared.vocal.mixToMono(), prepared.vocal.sampleRate, accompaniment, accompanimentRate);
    if (references.empty())
        return prepared;

    std::vector<ReferenceCandidate> candidates;
    for (const auto& argument : references)
    {
        ReferenceCandidate candidate;
        candidate.melody = loadReference (argument.path);
        candidate.name = candidate.melody.sourceName;
        candidate.offsetSeconds = argument.offset;
        if (! argument.offsetGiven)
        {
            const auto alignment = alignReference (analysis.notes, candidate.melody);
            candidate.offsetSeconds = alignment.offsetSeconds;
            appendf (log, "reference %s: %zu notes, auto offset %+.3f s (%zu/%zu sung notes match, %.0f%% of note time)\n",
                     candidate.name.c_str(), candidate.melody.notes.size(), candidate.offsetSeconds, alignment.matchedNotes,
                     alignment.consideredNotes, alignment.matchedFraction * 100.0);
        }
        candidates.push_back (std::move (candidate));
    }
    ReferenceSelectionSettings selectionSettings;
    selectionSettings.beatSeconds = bpmOverride >= 0.0 ? (bpmOverride > 0.0 ? 60.0 / bpmOverride : 0.0)
                                                       : beatSecondsForTempo (analysis.tempo.bpm, analysis.tempo.confidence);
    appendf (log, "tempo %.2f BPM (confidence %.2f)%s: beat %.3f s for shifted matching\n", analysis.tempo.bpm, analysis.tempo.confidence,
             bpmOverride >= 0.0 ? " overridden" : "", selectionSettings.beatSeconds);
    auto selection = selectReferences (analysis.notes, candidates, selectionSettings, choose);
    const auto alignment = alignNotesToReferences (analysis, candidates, selection);
    const auto stats = applySelectedReferences (analysis.notes, candidates, selection, selectionSettings);
    measureReach (analysis.notes, analysis.pitch);
    for (size_t c = 0; c < candidates.size(); ++c)
        appendf (log, "candidate %zu %s (offset %+.3f s): present for %.0f%% of sung time, loose match %.0f%% where present%s\n", c,
                 candidates[c].name.c_str(), candidates[c].offsetSeconds, selection.overallPresence[c] * 100.0,
                 selection.overallLooseMatch[c] * 100.0, (int) c == selection.fileCandidate ? "  <- file" : "");
    appendf (log, "aligned to the references: %zu phrases, %zu sung notes -> %zu parts (one per reference note aimed at)\n",
             alignment.phrases, alignment.notesBefore, alignment.partsAfter);
    appendf (log, "reference targets for %zu of %zu notes (%zu beyond 2.5 semitones, %zu unsure: kept on the scale)\n",
             stats.referenceTargets, analysis.notes.size(), stats.largeShifts, stats.unsure);
    size_t measured = 0, unreached = 0;
    double reachSum = 0.0;
    for (const auto& entry : analysis.notes)
        if (entry.reachFraction >= 0.0 && entry.note.endSeconds - entry.note.startSeconds >= 0.1)
        {
            ++measured;
            reachSum += entry.reachFraction;
            unreached += entry.reachFraction < 0.5 ? 1 : 0;
        }
    if (measured > 0)
        appendf (log, "reach (notes >= 100 ms with a reference target): %zu notes, mean %.0f%% of the note within 50 cents of the target, "
                      "%zu below 50%%\n",
                 measured, 100.0 * reachSum / (double) measured, unreached);
    if (describePhrases)
        log += describeSelection (selection, candidates);
    prepared.candidates = std::move (candidates);
    prepared.selection = std::move (selection);
    prepared.selectionSettings = selectionSettings;
    return prepared;
}

EditedAnalysis applyEdits (const PreparedCorrection& prepared, const NoteEditDocument& document)
{
    auto notes = applyNoteEdits (prepared.analysis, document, prepared.candidates, prepared.selection, prepared.selectionSettings);
    EditedAnalysis edited;
    edited.analysis = prepared.analysis;
    edited.analysis.notes = std::move (notes.notes);
    edited.overrides = std::move (notes.overrides);
    edited.unmatchedStructure = std::move (notes.unmatchedStructure);
    edited.unmatchedEdits = std::move (notes.unmatchedEdits);
    return edited;
}

std::string describeSummary (const SongAnalysis& analysis, const CorrectionCurve& curve, const CorrectionSettings& settings,
                             const std::string& outputName)
{
    size_t numCorrected = 0, numEdited = 0;
    for (const auto& note : curve.notes)
    {
        numCorrected += note.corrected ? 1 : 0;
        numEdited += note.edited ? 1 : 0;
    }
    std::string out;
    appendf (out, "key %s, %zu notes, %zu corrected (strength %.0f%%, threshold %.0f cents)",
             analysis.hasAccompaniment ? keyName (analysis.key.globalKey).c_str() : "(none: chromatic)", analysis.notes.size(), numCorrected,
             settings.strength * 100.0, settings.thresholdCents);
    if (numEdited > 0)
        appendf (out, ", %zu edited by hand", numEdited);
    appendf (out, " -> %s\n", outputName.c_str());
    return out;
}

std::string describeNotes (const SongAnalysis& analysis, const CorrectionCurve& curve, const CorrectionCurve* automatic,
                           const std::vector<NoteWithKey>* automaticNotes)
{
    // The automatic decision for note i: the automatic note overlapping it most (same index without automaticNotes).
    const auto automaticFor = [&] (size_t i) -> const NoteCorrection*
    {
        if (automatic == nullptr)
            return nullptr;
        if (automaticNotes == nullptr)
            return i < automatic->notes.size() ? &automatic->notes[i] : nullptr;
        const auto& note = analysis.notes[i].note;
        const NoteCorrection* best = nullptr;
        double bestOverlap = 0.0;
        for (size_t k = 0; k < automaticNotes->size() && k < automatic->notes.size(); ++k)
        {
            const auto& other = (*automaticNotes)[k].note;
            const double overlap = std::min (note.endSeconds, other.endSeconds) - std::max (note.startSeconds, other.startSeconds);
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = &automatic->notes[k];
            }
        }
        return best;
    };
    std::string out;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const auto& entry = analysis.notes[i];
        const auto& note = entry.note;
        const auto& decision = curve.notes[i];
        const char* origin = note.glide ? "(glide)" : entry.largeReferenceShift ? "(ref+)" : entry.hasReferenceTarget ? "(ref)" : entry.referenceUnsure ? "(key?)" : "(key)";
        char beats[48] = "";
        if (entry.hasReferenceTarget || entry.referenceUnsure)
            std::snprintf (beats, sizeof (beats), "  conf %.2f", entry.referenceConfidence);
        if (entry.hasReferenceTarget && entry.referenceShiftBeats != 0)
            std::snprintf (beats + std::strlen (beats), sizeof (beats) - std::strlen (beats), "  @%+d beats", entry.referenceShiftBeats);
        // Hand edits: the exact target (fractional MIDI, 1 cent = 0.01), or "keep".
        char edited[96] = "";
        if (decision.edited)
        {
            char automaticPart[48] = "";
            if (const auto* automaticDecision = automaticFor (i))
                std::snprintf (automaticPart, sizeof (automaticPart), "; auto %s %+.1fc", midiNoteName ((int) automaticDecision->targetMidi).c_str(),
                               automaticDecision->shiftSemitones * 100.0);
            if (decision.corrected)
                std::snprintf (edited, sizeof (edited), "  (edited: target %.2f%s)", decision.targetMidi, automaticPart);
            else
                std::snprintf (edited, sizeof (edited), "  (edited: keep%s)", automaticPart);
        }
        const int targetNote = decision.edited ? (int) std::lround (decision.targetMidi) : (int) decision.targetMidi;
        char reach[64] = "";
        if (entry.reachFraction >= 0.0)
            std::snprintf (reach, sizeof (reach), "  reach %.0f%%%s", entry.reachFraction * 100.0,
                           entry.settleSeconds >= 0.0 ? (" settle " + std::to_string (entry.settleSeconds).substr (0, 5) + "s").c_str() : " never");
        appendf (out, "  %7.3f-%7.3fs centre %7.3f target %-4s%-6s dev %+7.1fc shift %+7.1fc%s%s%s%s%s\n", note.startSeconds, note.endSeconds,
                 note.centreMidi, midiNoteName (targetNote).c_str(), origin, decision.deviationCents, decision.shiftSemitones * 100.0,
                 decision.follows ? "  (follows)" : decision.corrected ? "" : "  (kept)", decision.curve ? "  (curve)" : "", beats, reach,
                 (std::string (edited) + (entry.alignedReferenceNote == -2 ? "  (split/merged)" : "")).c_str());
    }
    return out;
}

} // namespace shizuku::tools
