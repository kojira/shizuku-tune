#include "ReferenceSet.h"

namespace shizuku
{

using namespace juce;

String ReferenceSet::add (const File& file)
{
    try
    {
        auto melody = std::make_shared<ReferenceMelody> (loadReference (file.getFullPathName().toStdString()));
        if (melody->notes.empty())
            return String::fromUTF8 (u8"正解のメロディーに音がありません。");
        melodies.push_back (std::move (melody));
        alignments.emplace_back();
        paths.add (file.getFullPathName());
    }
    catch (const std::exception& error)
    {
        return String (error.what());
    }
    return {};
}

void ReferenceSet::clear()
{
    melodies.clear();
    alignments.clear();
    paths.clear();
}

ReferenceTargets ReferenceSet::applyTo (const TrackAnalysisResult& base, int choice)
{
    ReferenceTargets result;
    auto& analysis = result.analysis;
    analysis = base.analysis;
    for (auto& entry : analysis.notes)
    {
        entry.nearestScaleMidi = analysis.hasAccompaniment ? nearestScaleMidiNote (entry.keyAtNote, entry.note.centreMidi) : entry.note.nearestMidi;
        entry.hasReferenceTarget = false;
    }
    if (melodies.empty())
        return result;

    std::vector<ReferenceCandidate> candidates;
    for (size_t c = 0; c < melodies.size(); ++c)
    {
        const auto& melody = *melodies[c];
        const auto alignment = alignReference (analysis.notes, melody);
        auto& best = alignments[c];
        if (alignment.matchedNotes > best.matchedNotes) // most evidence, not the highest fraction of a short part
            best = alignment;
        ReferenceCandidate candidate;
        candidate.name = melody.sourceName;
        candidate.melody = melody;
        candidate.offsetSeconds = best.offsetSeconds;
        candidates.push_back (std::move (candidate));
    }
    ReferenceSelectionSettings selectionSettings;
    selectionSettings.beatSeconds = beatSecondsForTempo (analysis.tempo.bpm, analysis.tempo.confidence);
    result.beatSeconds = selectionSettings.beatSeconds;
    result.tempoBpm = analysis.tempo.bpm;
    auto selection = selectReferences (analysis.notes, candidates, selectionSettings, choice == referenceAuto ? -2 : choice);
    if (choice != referenceNone)
    {
        // One part per reference note aimed at, corrected along the target pitch curve (as shizuku-correct).
        alignNotesToReferences (analysis, candidates, selection);
        result.stats = applySelectedReferences (analysis.notes, candidates, selection, selectionSettings);
        measureReach (analysis.notes, analysis.pitch);
    }
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        ReferenceCandidateInfo info;
        info.name = String::fromUTF8 (candidates[c].name.c_str());
        info.offsetSeconds = candidates[c].offsetSeconds;
        info.presence = selection.overallPresence[c];
        info.looseMatch = selection.overallLooseMatch[c];
        for (const auto& segment : selection.segments)
            if (segment.candidate == (int) c)
            {
                info.usedSeconds += segment.endSeconds - segment.startSeconds;
                if (segment.chosenShiftBeats() != 0)
                {
                    info.shiftedSeconds += segment.endSeconds - segment.startSeconds;
                    info.shiftedPhrases.add (String (base.timelineStartSeconds + segment.startSeconds, 1) + " s "
                                             + (segment.chosenShiftBeats() > 0 ? "+" : "") + String (segment.chosenShiftBeats()) + " beats");
                }
            }
        result.candidates.push_back (info);
    }
    result.referencesApplied = choice != referenceNone;
    result.referenceCandidates = candidates;
    result.selection = std::move (selection);
    result.selectionSettings = selectionSettings;
    return result;
}

String ReferenceSet::describe (const ReferenceTargets* targets, int choice, bool japanese) const
{
    if (japanese)
    {
        if (melodies.empty())
            return String::fromUTF8 (u8"正解なし（キーの音階に寄せます）。ガイドや主旋律を「正解を追加…」で読み込めます。");
        String text = String::fromUTF8 (u8"正解の候補 ") + String ((int) melodies.size()) + String::fromUTF8 (u8" 件");
        if (targets == nullptr)
            return text;
        const auto& stats = targets->stats;
        text << String::fromUTF8 (u8"、正解を使う音 ") << (int) stats.referenceTargets << String::fromUTF8 (u8"（2.5 半音を超える ")
             << (int) stats.largeShifts << String::fromUTF8 (u8"）、自信が低く音階のままの音 ") << (int) stats.unsure;
        if (targets->beatSeconds > 0.0)
        {
            text << String::fromUTF8 (u8"、テンポ ") << String (targets->tempoBpm, 1) << " BPM";
            StringArray shifted;
            for (const auto& info : targets->candidates)
                for (const auto& phrase : info.shiftedPhrases)
                    shifted.add (info.name + " " + phrase);
            if (! shifted.isEmpty())
                text << String::fromUTF8 (u8"、拍ずらし: ") << shifted.joinIntoString (", ");
        }
        return text;
    }
    if (melodies.empty())
        return "No reference melody (targets: key scale). Add guide / original-lead melodies as candidates.";
    String text = String ((int) melodies.size()) + " reference candidate(s); ";
    text << (choice == referenceAuto ? "chosen per phrase" : choice == referenceNone ? "not used (key scale)" : "fixed choice");
    if (targets == nullptr)
        return text;
    const auto& stats = targets->stats;
    text << ": " << (int) stats.referenceTargets << " notes use a reference (" << (int) stats.largeShifts << " beyond 2.5 semitones), "
         << (int) stats.unsure << " unsure (kept on the scale, marked red)";
    if (targets->beatSeconds > 0.0)
    {
        text << "; tempo " << String (targets->tempoBpm, 1) << " BPM";
        StringArray shifted;
        for (const auto& info : targets->candidates)
            for (const auto& phrase : info.shiftedPhrases)
                shifted.add (info.name + " " + phrase);
        if (! shifted.isEmpty())
            text << "; call and response (reference used later/earlier by whole beats): " << shifted.joinIntoString (", ");
    }
    return text;
}

} // namespace shizuku
