#include "ReferenceSelection.h"

#include "MusicTheory.h"
#include "NoteSegmenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>

namespace shizuku
{

namespace
{
constexpr double matchSlackSeconds = 0.05;  // timing tolerance when matching a sung note to reference notes
constexpr double targetSlackSeconds = 0.1;  // ... when looking for a near reference note to use as the target
constexpr double maxReferenceNoteSeconds = 8.0;
constexpr double onsetWindowSeconds = 0.3;  // reference onsets searched around a sung onset
constexpr double onsetScaleSeconds = 0.15;  // onset agreement falls linearly to 0 at this distance

double noteLength (const NoteWithKey& entry) { return entry.note.endSeconds - entry.note.startSeconds; }

// Reference notes overlapping [from, to] (vocal time), with the candidate placed at `offset` (vocal time =
// reference time + offset; the candidate's own offset plus any beat shift).
std::vector<const ReferenceNote*> notesOverlapping (const ReferenceCandidate& candidate, double offset, double from, double to)
{
    std::vector<const ReferenceNote*> found;
    const auto& notes = candidate.melody.notes;
    const double refFrom = from - offset, refTo = to - offset;
    auto it = std::upper_bound (notes.begin(), notes.end(), refTo, [] (double t, const ReferenceNote& n) { return t < n.startSeconds; });
    while (it != notes.begin())
    {
        --it;
        if (it->endSeconds > refFrom && it->startSeconds < refTo)
            found.push_back (&*it);
        if (refFrom - it->startSeconds > maxReferenceNoteSeconds)
            break;
    }
    return found;
}

double overlapSeconds (double offset, const ReferenceNote& reference, double from, double to)
{
    return std::max (0.0, std::min (to, reference.endSeconds + offset) - std::max (from, reference.startSeconds + offset));
}

// Offset of a candidate in a segment: its alignment plus the segment's beat shift for it.
double segmentOffset (const ReferenceCandidate& candidate, int shiftBeats, double beatSeconds)
{
    return candidate.offsetSeconds + shiftBeats * beatSeconds;
}

struct NoteMatch
{
    bool present = false;  // a reference note sounds during the sung note
    double error = 99.0;   // |sung - reference| at the best octave, semitones (any note within the timing slack)
    int octave = 0;        // sung - reference, octaves
    int part = -1;
    double mainError = 99.0;     // |sung - main reference note| at the nearest octave (the note a target would use)
    double onsetDistance = 99.0; // |sung onset - nearest reference onset|, seconds
};

const ReferenceNote* mainNote (const ReferenceCandidate& candidate, double offset, double from, double to, double& overlap);

NoteMatch matchNote (const NoteWithKey& entry, const ReferenceCandidate& candidate, double offset)
{
    NoteMatch match;
    const auto& note = entry.note;
    match.present = ! notesOverlapping (candidate, offset, note.startSeconds, note.endSeconds).empty();
    double overlap = 0.0;
    if (const auto* main = mainNote (candidate, offset, note.startSeconds, note.endSeconds, overlap))
    {
        const double difference = note.centreMidi - main->midi;
        match.mainError = std::abs (difference - 12.0 * std::round (difference / 12.0));
    }
    for (const auto* reference : notesOverlapping (candidate, offset, note.startSeconds - onsetWindowSeconds, note.startSeconds + onsetWindowSeconds))
        if (reference->startSeconds + offset >= note.startSeconds - onsetWindowSeconds)
            match.onsetDistance = std::min (match.onsetDistance, std::abs (reference->startSeconds + offset - note.startSeconds));
    for (const auto* reference : notesOverlapping (candidate, offset, note.startSeconds - matchSlackSeconds, note.endSeconds + matchSlackSeconds))
    {
        const double difference = note.centreMidi - reference->midi;
        const int octave = (int) std::lround (difference / 12.0);
        const double error = std::abs (difference - 12.0 * octave);
        if (error < match.error)
        {
            match.error = error;
            match.octave = octave;
            match.part = reference->part;
        }
    }
    return match;
}

// The reference note sounding longest during [from, to] (ties: the later-starting one), and that overlap.
const ReferenceNote* mainNote (const ReferenceCandidate& candidate, double offset, double from, double to, double& overlap)
{
    const ReferenceNote* main = nullptr;
    overlap = 0.0;
    for (const auto* reference : notesOverlapping (candidate, offset, from, to))
    {
        const double length = overlapSeconds (offset, *reference, from, to);
        if (length > overlap)
        {
            overlap = length;
            main = reference;
        }
    }
    return main;
}

// Signed distance (semitones) from `sung` to the nearest octave of `referenceMidi`, in [-6, 6).
double octaveFoldedDistance (double sung, int referenceMidi)
{
    double distance = std::fmod (sung - referenceMidi, 12.0);
    if (distance < -6.0)
        distance += 12.0;
    if (distance >= 6.0)
        distance -= 12.0;
    return distance;
}

// The main reference note of a sung note: its aligned note (alignNotesToReferences; it counts as covering the
// whole note), otherwise the one sounding longest during it.
const ReferenceNote* alignedMainNote (const NoteWithKey& entry, const ReferenceCandidate& candidate, double offset, double& overlap)
{
    const auto& notes = candidate.melody.notes;
    if (entry.alignedReferenceNote >= 0 && (size_t) entry.alignedReferenceNote < notes.size())
    {
        overlap = entry.note.endSeconds - entry.note.startSeconds;
        return &notes[(size_t) entry.alignedReferenceNote];
    }
    return mainNote (candidate, offset, entry.note.startSeconds, entry.note.endSeconds, overlap);
}

template <typename Key>
Key mostWeighted (const std::map<Key, double>& weights, Key fallback)
{
    Key best = fallback;
    double bestWeight = 0.0;
    for (const auto& [key, weight] : weights)
        if (weight > bestWeight)
        {
            best = key;
            bestWeight = weight;
        }
    return best;
}
} // namespace

double ReferenceSelectionSettings::nearLimitForConfidence (double confidence) const
{
    if (confidence >= 1.0 || confidence >= fullConfidence)
        return nearSemitones;
    if (confidence <= lowConfidence || fullConfidence <= lowConfidence)
        return std::min (nearSemitones, lowConfidenceSemitones);
    const double t = (confidence - lowConfidence) / (fullConfidence - lowConfidence);
    return std::min (nearSemitones, lowConfidenceSemitones + t * (nearSemitones - lowConfidenceSemitones));
}

double beatSecondsForTempo (double bpm, double confidence)
{
    return bpm > 0.0 && confidence >= 0.1 ? 60.0 / bpm : 0.0;
}

ReferenceSelection selectReferences (const std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                     const ReferenceSelectionSettings& settings, int forcedCandidate)
{
    ReferenceSelection selection;
    selection.beatSeconds = std::max (0.0, settings.beatSeconds);
    const auto numCandidates = candidates.size();
    selection.overallLooseMatch.assign (numCandidates, 0.0);
    selection.overallPresence.assign (numCandidates, 0.0);
    if (notes.empty() || numCandidates == 0)
        return selection;

    // Shifts tried: 0, then +-1..maxShiftBeats beats (index s <-> shift shifts[s]).
    std::vector<int> shifts { 0 };
    if (selection.beatSeconds > 0.0)
        for (int k = 1; k <= settings.maxShiftBeats; ++k)
        {
            shifts.push_back (k);
            shifts.push_back (-k);
        }

    // Per note, candidate and shift.
    std::vector<std::vector<std::vector<NoteMatch>>> matches (notes.size(), std::vector<std::vector<NoteMatch>> (numCandidates));
    for (size_t i = 0; i < notes.size(); ++i)
        for (size_t c = 0; c < numCandidates; ++c)
            for (int shift : shifts)
                matches[i][c].push_back (matchNote (notes[i], candidates[c], segmentOffset (candidates[c], shift, selection.beatSeconds)));

    // Segments: runs of notes with gaps <= phraseGapSeconds.
    for (size_t i = 0; i < notes.size();)
    {
        ReferenceSegmentChoice segment;
        segment.firstNote = i;
        segment.startSeconds = notes[i].note.startSeconds;
        size_t j = i + 1;
        while (j < notes.size() && notes[j].note.startSeconds - notes[j - 1].note.endSeconds <= settings.phraseGapSeconds)
            ++j;
        segment.endNote = j;
        segment.endSeconds = notes[j - 1].note.endSeconds;
        selection.segments.push_back (segment);
        i = j;
    }

    // strict / loose / presence as above; shiftScore ranks beat shifts: per sung note (weighted by length) the
    // pitch agreement with the note a target would use (1 at 0, 0 at the loose tolerance) plus the onset
    // agreement with the nearest reference onset (1 at 0 s, 0 at onsetScaleSeconds). Stepwise lines match a
    // neighbouring beat almost as well in pitch; the onsets tell them apart.
    struct Fractions
    {
        double strict = 0.0, loose = 0.0, presence = 0.0, shiftScore = 0.0;
        double mainLoose = 0.0; // loose match against the note a target would use (no timing slack)
    };
    const auto fractionsFor = [&] (const ReferenceSegmentChoice& segment, size_t c, size_t shiftIndex)
    {
        Fractions f;
        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
        {
            const double length = noteLength (notes[i]);
            const auto& match = matches[i][c][shiftIndex];
            f.presence += match.present ? length : 0.0;
            f.strict += match.error < settings.strictToleranceSemitones ? length : 0.0;
            f.loose += match.error < settings.looseToleranceSemitones ? length : 0.0;
            f.mainLoose += match.mainError < settings.looseToleranceSemitones ? length : 0.0;
            f.shiftScore += length * (std::max (0.0, 1.0 - match.mainError / settings.looseToleranceSemitones)
                                      + std::max (0.0, 1.0 - match.onsetDistance / onsetScaleSeconds));
        }
        if (segment.sungSeconds > 0.0)
        {
            f.strict /= segment.sungSeconds;
            f.loose /= segment.sungSeconds;
            f.presence /= segment.sungSeconds;
            f.shiftScore /= segment.sungSeconds;
            f.mainLoose /= segment.sungSeconds;
        }
        return f;
    };

    std::vector<double> totalLoose (numCandidates, 0.0), totalPresent (numCandidates, 0.0);
    double totalSung = 0.0;
    for (auto& segment : selection.segments)
    {
        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
            segment.sungSeconds += noteLength (notes[i]);
        totalSung += segment.sungSeconds;
        segment.strictMatch.assign (numCandidates, 0.0);
        segment.looseMatch.assign (numCandidates, 0.0);
        segment.presence.assign (numCandidates, 0.0);
        segment.shiftBeats.assign (numCandidates, 0);
        segment.shiftScore.assign (numCandidates, 0.0);

        // Unshifted first (also the file-level statistics), then the shifts that clearly beat every unshifted match.
        double unshiftedBestMainLoose = 0.0;
        for (size_t c = 0; c < numCandidates; ++c)
        {
            const auto f = fractionsFor (segment, c, 0);
            segment.strictMatch[c] = f.strict;
            segment.looseMatch[c] = f.loose;
            segment.presence[c] = f.presence;
            segment.shiftScore[c] = f.shiftScore;
            totalLoose[c] += f.loose * segment.sungSeconds;
            totalPresent[c] += f.presence * segment.sungSeconds;
            if (forcedCandidate == -2 || (int) c == forcedCandidate)
            {
                segment.unshiftedBestLooseMatch = std::max (segment.unshiftedBestLooseMatch, f.loose);
                unshiftedBestMainLoose = std::max (unshiftedBestMainLoose, f.mainLoose);
            }
        }
        if (shifts.size() < 2 || segment.sungSeconds < settings.minShiftSungSeconds || segment.endNote - segment.firstNote < settings.minShiftNotes)
            continue;
        for (size_t c = 0; c < numCandidates; ++c)
        {
            double bestScore = -1.0;
            for (size_t s = 1; s < shifts.size(); ++s)
            {
                const auto f = fractionsFor (segment, c, s);
                // The loose match must clear the bar and gain over every unshifted candidate; the match against the
                // notes a target would use must not be worse than unshifted.
                if (f.loose < std::max (settings.minShiftMatch, settings.minSegmentMatch) || f.presence < settings.minPresence
                    || f.loose < segment.unshiftedBestLooseMatch + settings.minShiftGain - 1.0e-9
                    || f.mainLoose < unshiftedBestMainLoose - 1.0e-9 || f.shiftScore <= bestScore + 1.0e-9)
                    continue;
                bestScore = f.shiftScore;
                segment.shiftBeats[c] = shifts[s];
                segment.strictMatch[c] = f.strict;
                segment.looseMatch[c] = f.loose;
                segment.presence[c] = f.presence;
                segment.shiftScore[c] = f.shiftScore;
            }
        }
    }
    for (size_t c = 0; c < numCandidates; ++c)
    {
        selection.overallLooseMatch[c] = totalPresent[c] > 0.0 ? totalLoose[c] / totalPresent[c] : 0.0;
        selection.overallPresence[c] = totalSung > 0.0 ? totalPresent[c] / totalSung : 0.0;
    }

    // File-level candidate: the most loosely matched sung time, if it matches at least 30 % where present.
    if (forcedCandidate >= 0 && forcedCandidate < (int) numCandidates)
    {
        selection.fileCandidate = forcedCandidate;
    }
    else if (forcedCandidate == -2)
    {
        double best = 0.0;
        for (size_t c = 0; c < numCandidates; ++c)
            if (totalLoose[c] > best && selection.overallLooseMatch[c] >= 0.3)
            {
                best = totalLoose[c];
                selection.fileCandidate = (int) c;
            }
    }

    const auto shiftIndexOf = [&] (int shift)
    { return (size_t) (std::find (shifts.begin(), shifts.end(), shift) - shifts.begin()); };

    // Ranking of candidates within a segment: loose match, then strict match, then the file-level candidate.
    // When a shifted candidate is involved, shiftScore (pitch of the target note + onsets) comes first.
    const auto better = [&] (const ReferenceSegmentChoice& segment, size_t c, size_t current)
    {
        const bool shifted = segment.shiftBeats[c] != 0 || segment.shiftBeats[current] != 0;
        const auto key = [&] (size_t k)
        {
            return std::make_tuple (shifted ? std::round (segment.shiftScore[k] * 1.0e6) : 0.0, std::round (segment.looseMatch[k] * 1.0e6),
                                    std::round (segment.strictMatch[k] * 1.0e6), (int) k == selection.fileCandidate ? 1 : 0,
                                    -std::abs (segment.shiftBeats[k]));
        };
        return key (c) > key (current);
    };

    for (auto& segment : selection.segments)
    {
        if (forcedCandidate == -2)
            for (size_t c = 0; c < numCandidates; ++c)
            {
                if (segment.presence[c] < settings.minPresence || segment.looseMatch[c] < settings.minSegmentMatch)
                    continue;
                if (segment.candidate < 0 || better (segment, c, (size_t) segment.candidate))
                {
                    segment.candidate = (int) c;
                    segment.matched = true;
                }
            }
        else if (forcedCandidate >= 0 && forcedCandidate < (int) numCandidates && segment.shiftBeats[(size_t) forcedCandidate] != 0)
        {
            segment.candidate = forcedCandidate; // a shifted match of the forced candidate is present by construction
            segment.matched = true;
        }
        if (segment.candidate < 0 && selection.fileCandidate >= 0 && segment.presence[(size_t) selection.fileCandidate] >= settings.minPresence)
        {
            segment.candidate = selection.fileCandidate;
            segment.matched = segment.looseMatch[(size_t) segment.candidate] >= settings.minSegmentMatch;
        }
        if (segment.candidate < 0)
            continue;

        const auto shiftIndex = shiftIndexOf (segment.chosenShiftBeats());
        std::map<int, double> octaves, parts;
        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
        {
            const auto& match = matches[i][(size_t) segment.candidate][shiftIndex];
            if (match.error < settings.looseToleranceSemitones)
            {
                octaves[match.octave] += noteLength (notes[i]);
                parts[match.part] += noteLength (notes[i]);
            }
        }
        segment.octave = mostWeighted (octaves, 0);
        segment.part = mostWeighted (parts, -1);
    }

    // Segments without a matching note take the octave of the nearest segment of the same candidate that has one.
    for (size_t s = 0; s < selection.segments.size(); ++s)
    {
        auto& segment = selection.segments[s];
        if (segment.candidate < 0 || segment.looseMatch[(size_t) segment.candidate] > 0.0)
            continue;
        double bestDistance = 1.0e9;
        for (const auto& other : selection.segments)
            if (&other != &segment && other.candidate == segment.candidate && other.looseMatch[(size_t) other.candidate] > 0.0)
            {
                const double distance = std::abs (0.5 * (other.startSeconds + other.endSeconds) - 0.5 * (segment.startSeconds + segment.endSeconds));
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    segment.octave = other.octave;
                    segment.part = other.part;
                }
            }
    }
    return selection;
}

ReferenceApplyStats applySelectedReferences (std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                             const ReferenceSelection& selection, const ReferenceSelectionSettings& settings)
{
    ReferenceApplyStats stats;
    for (const auto& segment : selection.segments)
    {
        if (segment.candidate < 0 || segment.endNote > notes.size())
            continue;
        const auto& candidate = candidates[(size_t) segment.candidate];
        const double offset = segmentOffset (candidate, segment.chosenShiftBeats(), selection.beatSeconds);

        // Loose match of this candidate over the sung time of the other segments around. The segment itself is
        // left out (a whole phrase sung off is what a large shift has to fix) unless there is too little singing
        // around; then this segment without the note itself is used.
        const double middle = 0.5 * (segment.startSeconds + segment.endSeconds);
        double matchedAround = 0.0, sungAround = 0.0;
        for (const auto& other : selection.segments)
            if (&other != &segment && std::abs (0.5 * (other.startSeconds + other.endSeconds) - middle) <= settings.neighbourSeconds)
            {
                sungAround += other.sungSeconds;
                matchedAround += other.sungSeconds * other.looseMatch[(size_t) segment.candidate];
            }
        const bool useOwnSegment = sungAround < settings.minNeighbourSungSeconds;
        if (useOwnSegment)
        {
            sungAround = segment.sungSeconds;
            matchedAround = segment.sungSeconds * segment.looseMatch[(size_t) segment.candidate];
        }

        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
        {
            auto& entry = notes[i];
            const auto& note = entry.note;
            const double length = noteLength (entry);
            const auto around = notesOverlapping (candidate, offset, note.startSeconds - targetSlackSeconds, note.endSeconds + targetSlackSeconds);
            if (around.empty() && entry.alignedReferenceNote < 0)
                continue;

            // Main: the aligned reference note, or the reference note sounding longest during the sung note.
            double mainOverlap = 0.0;
            const ReferenceNote* main = alignedMainNote (entry, candidate, offset, mainOverlap);

            // Near: the main note; without one (sung note in a gap of the reference), the closest note within
            // the timing slack. Nearest octave.
            const ReferenceNote* near = nullptr;
            double nearDistance = 0.0;
            if (main != nullptr)
            {
                if (std::abs (octaveFoldedDistance (note.centreMidi, main->midi)) <= settings.nearLimitForConfidence (main->confidence))
                {
                    near = main;
                    nearDistance = octaveFoldedDistance (note.centreMidi, main->midi);
                }
            }
            else
            {
                for (const auto* reference : around)
                {
                    const double distance = octaveFoldedDistance (note.centreMidi, reference->midi);
                    if (std::abs (distance) <= settings.nearLimitForConfidence (reference->confidence)
                        && (near == nullptr || std::abs (distance) < std::abs (nearDistance)))
                    {
                        near = reference;
                        nearDistance = distance;
                    }
                }
            }
            if (near != nullptr)
            {
                entry.nearestScaleMidi = (int) std::lround (note.centreMidi - nearDistance);
                entry.hasReferenceTarget = true;
                entry.referenceCandidate = segment.candidate;
                entry.referencePart = near->part;
                entry.referenceConfidence = near->confidence;
                entry.referenceShiftBeats = segment.chosenShiftBeats();
                entry.referenceCurve = entry.alignedReferenceNote != -1;
                ++stats.referenceTargets;
                continue;
            }

            // Large: the main note at the segment's octave, when the reference is trustworthy here.
            if (main != nullptr && mainOverlap >= 0.5 * length && (main->confidence >= 1.0 || length >= settings.largeMinNoteSeconds))
            {
                // In the own-segment case the note itself is far off (not a loose match): it only leaves the denominator.
                const double rest = useOwnSegment ? sungAround - length : sungAround;
                const double neighbourMatch = rest > 0.0 ? matchedAround / rest : 0.0;
                const int target = main->midi + 12 * segment.octave;
                const bool trusted = main->confidence >= settings.largeMinNoteConfidence && neighbourMatch >= settings.largeMinNeighbourMatch;
                if (trusted && std::abs (note.centreMidi - target) <= settings.largeSemitones)
                {
                    entry.nearestScaleMidi = target;
                    entry.hasReferenceTarget = true;
                    entry.largeReferenceShift = true;
                    entry.referenceCandidate = segment.candidate;
                    entry.referencePart = main->part;
                    entry.referenceConfidence = main->confidence;
                    entry.referenceShiftBeats = segment.chosenShiftBeats();
                    entry.referenceCurve = entry.alignedReferenceNote != -1;
                    ++stats.referenceTargets;
                    ++stats.largeShifts;
                    continue;
                }
            }
            if (main != nullptr)
            {
                entry.referenceUnsure = true;
                entry.referenceConfidence = main->confidence;
                ++stats.unsure;
            }
        }
    }
    return stats;
}

std::vector<double> referencePitchForErrors (const std::vector<NoteWithKey>& notes, const std::vector<ReferenceCandidate>& candidates,
                                             const ReferenceSelection& selection)
{
    std::vector<double> result (notes.size(), std::nan (""));
    for (const auto& segment : selection.segments)
    {
        if (segment.candidate < 0 || segment.endNote > notes.size())
            continue;
        const auto& candidate = candidates[(size_t) segment.candidate];
        const double offset = segmentOffset (candidate, segment.chosenShiftBeats(), selection.beatSeconds);
        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
        {
            const auto& note = notes[i].note;
            double mainOverlap = 0.0;
            const ReferenceNote* main = alignedMainNote (notes[i], candidate, offset, mainOverlap);
            if (main == nullptr || mainOverlap < 0.5 * (note.endSeconds - note.startSeconds))
                continue;
            const double atSegmentOctave = main->midi + 12.0 * segment.octave;
            const double atNearestOctave = note.centreMidi - octaveFoldedDistance (note.centreMidi, main->midi);
            result[i] = std::abs (note.centreMidi - atSegmentOctave) > 6.0 ? atNearestOctave : atSegmentOctave;
        }
    }
    return result;
}

void assignNotesToSegments (ReferenceSelection& selection, const std::vector<NoteWithKey>& notes)
{
    // Each note belongs to the phrase it overlaps most (none: the nearest one before it keeps the range contiguous).
    std::vector<long> owner (notes.size(), -1);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        double best = 0.0;
        for (size_t s = 0; s < selection.segments.size(); ++s)
        {
            const auto& segment = selection.segments[s];
            const double overlap = std::min (segment.endSeconds, notes[i].note.endSeconds) - std::max (segment.startSeconds, notes[i].note.startSeconds);
            if (overlap > best || (overlap >= 0.0 && best <= 0.0 && owner[i] < 0))
            {
                best = std::max (best, overlap);
                owner[i] = (long) s;
            }
        }
    }
    for (size_t s = 0; s < selection.segments.size(); ++s)
    {
        auto& segment = selection.segments[s];
        segment.firstNote = segment.endNote = 0;
        bool any = false;
        for (size_t i = 0; i < notes.size(); ++i)
            if (owner[i] == (long) s)
            {
                if (! any)
                    segment.firstNote = i;
                segment.endNote = i + 1;
                any = true;
            }
    }
}

NoteAlignmentStats alignNotesToReferences (SongAnalysis& analysis, const std::vector<ReferenceCandidate>& candidates,
                                           ReferenceSelection& selection, const NoteAlignmentSettings& settings)
{
    NoteAlignmentStats stats;
    const auto& track = analysis.pitch;
    const auto& frames = track.frames;
    const double hop = track.hopSeconds;
    auto& notes = analysis.notes;
    if (frames.empty() || hop <= 0.0 || notes.empty())
        return stats;
    const auto smooth = smoothedPitchMidi (track, settings.smoothSeconds);
    const auto frameAt = [&] (double t) { return (long) std::lround ((t - frames.front().timeSeconds) / hop); };
    const auto minPartFrames = (size_t) std::max (1L, (long) std::lround (settings.minPartSeconds / hop));

    std::vector<NoteWithKey> result;
    size_t next = 0; // notes before it are in result
    for (const auto& segment : selection.segments)
    {
        if (segment.firstNote < next || segment.endNote > notes.size())
            continue;
        for (; next < segment.firstNote; ++next)
            result.push_back (notes[next]);
        if (segment.candidate < 0 || segment.endNote <= segment.firstNote)
            continue;
        const auto& candidate = candidates[(size_t) segment.candidate];
        const double offset = segmentOffset (candidate, segment.chosenShiftBeats(), selection.beatSeconds);
        const auto& melody = candidate.melody.notes;

        // Reference notes around the phrase (indices into the melody).
        std::vector<size_t> refs;
        for (size_t r = 0; r < melody.size(); ++r)
            if (melody[r].endSeconds + offset > segment.startSeconds - 0.3 && melody[r].startSeconds + offset < segment.endSeconds + 0.3)
                refs.push_back (r);
        // Sung frames of the phrase: voiced frames inside its notes.
        struct SungFrame
        {
            size_t frame, note;
        };
        std::vector<SungFrame> sung;
        for (size_t i = segment.firstNote; i < segment.endNote; ++i)
        {
            const long first = std::max (0L, frameAt (notes[i].note.startSeconds + 0.5 * hop));
            const long last = std::min ((long) frames.size() - 1, frameAt (notes[i].note.endSeconds - 0.5 * hop));
            for (long f = first; f <= last; ++f)
                if (frames[(size_t) f].voiced)
                    sung.push_back ({ (size_t) f, i });
        }
        if (refs.empty() || sung.empty())
        {
            for (; next < segment.endNote; ++next)
                result.push_back (notes[next]);
            continue;
        }

        // Dynamic time warping: frame k on reference note j (monotonic).
        const size_t numRefs = refs.size(), numFrames = sung.size();
        const auto cost = [&] (size_t k, size_t j)
        {
            const auto& reference = melody[refs[j]];
            const double t = frames[sung[k].frame].timeSeconds;
            const double pitch = std::min (settings.maxPitchCost, std::abs (octaveFoldedDistance (smooth[sung[k].frame], reference.midi)));
            const double outside = std::max ({ 0.0, reference.startSeconds + offset - settings.timeSlackSeconds - t,
                                               t - (reference.endSeconds + offset + settings.timeSlackSeconds) });
            return pitch + settings.timeWeightPerSecond * outside;
        };
        constexpr double infinity = std::numeric_limits<double>::infinity();
        std::vector<double> total (numFrames * numRefs, infinity);
        std::vector<int> from (numFrames * numRefs, -1);
        for (size_t j = 0; j < numRefs; ++j)
            total[j] = cost (0, j);
        for (size_t k = 1; k < numFrames; ++k)
        {
            const bool sameNote = sung[k].note == sung[k - 1].note;
            for (size_t j = 0; j < numRefs; ++j)
            {
                double best = total[(k - 1) * numRefs + j];
                int bestFrom = (int) j;
                for (size_t p = 0; p < j; ++p)
                {
                    const double value = total[(k - 1) * numRefs + p] + settings.skipCost * (double) (j - p - 1)
                                         + (sameNote ? settings.changeInsideNoteCost : 0.0);
                    if (value < best)
                    {
                        best = value;
                        bestFrom = (int) p;
                    }
                }
                total[k * numRefs + j] = best + cost (k, j);
                from[k * numRefs + j] = bestFrom;
            }
        }
        std::vector<size_t> assigned (numFrames);
        size_t j = (size_t) (std::min_element (total.begin() + (long) ((numFrames - 1) * numRefs), total.end())
                             - (total.begin() + (long) ((numFrames - 1) * numRefs)));
        for (size_t k = numFrames; k-- > 0;)
        {
            assigned[k] = j;
            if (k > 0)
                j = (size_t) from[k * numRefs + j];
        }

        // Runs of one reference note within one sung note; short runs join the previous (or next) run of the note.
        struct Run
        {
            size_t begin, end; // [begin, end) in sung
            size_t ref;
        };
        std::vector<Run> runs;
        for (size_t k = 0; k < numFrames; ++k)
        {
            if (! runs.empty() && runs.back().ref == assigned[k] && sung[runs.back().begin].note == sung[k].note)
                runs.back().end = k + 1;
            else
                runs.push_back ({ k, k + 1, assigned[k] });
        }
        for (bool changed = true; changed;)
        {
            changed = false;
            for (size_t r = 0; r < runs.size(); ++r)
            {
                if (runs[r].end - runs[r].begin >= minPartFrames)
                    continue;
                const size_t note = sung[runs[r].begin].note;
                const bool previous = r > 0 && sung[runs[r - 1].begin].note == note;
                const bool following = r + 1 < runs.size() && sung[runs[r + 1].begin].note == note;
                if (! previous && ! following)
                    continue;
                // Join the longer neighbour of the same note; it keeps its reference note.
                const bool toPrevious = previous && (! following || runs[r - 1].end - runs[r - 1].begin >= runs[r + 1].end - runs[r + 1].begin);
                if (toPrevious)
                    runs[r - 1].end = runs[r].end;
                else
                    runs[r + 1].begin = runs[r].begin;
                runs.erase (runs.begin() + (long) r);
                changed = true;
                break;
            }
        }
        // Consecutive runs on the same reference note with a short gap become one part (pieces of one note).
        std::vector<Run> parts;
        for (const auto& run : runs)
        {
            if (! parts.empty() && parts.back().ref == run.ref
                && frames[sung[run.begin].frame].timeSeconds - frames[sung[parts.back().end - 1].frame].timeSeconds - hop < settings.mergeGapSeconds)
                parts.back().end = run.end;
            else
                parts.push_back (run);
        }

        for (const auto& part : parts)
        {
            NoteWithKey entry;
            entry.note = noteFromFrames (track, sung[part.begin].frame, sung[part.end - 1].frame + 1);
            if (entry.note.endSeconds <= entry.note.startSeconds)
                continue;
            entry.keyAtNote = keyAtTime (analysis.key, 0.5 * (entry.note.startSeconds + entry.note.endSeconds));
            entry.nearestScaleMidi = analysis.hasAccompaniment ? nearestScaleMidiNote (entry.keyAtNote, entry.note.centreMidi) : entry.note.nearestMidi;
            // A part whose middle is not inside its reference note (+-alignedMiddleSlackSeconds) is not clearly
            // aimed at it (e.g. a note sung on its own just after the reference note ended): it keeps the time-overlap
            // rules of an unaligned note.
            const auto& reference = melody[refs[part.ref]];
            const double middle = 0.5 * (entry.note.startSeconds + entry.note.endSeconds);
            const bool inside = middle >= reference.startSeconds + offset - settings.alignedMiddleSlackSeconds
                                && middle <= reference.endSeconds + offset + settings.alignedMiddleSlackSeconds;
            entry.alignedReferenceNote = inside ? (long) refs[part.ref] : -1;
            result.push_back (entry);
            ++stats.partsAfter;
        }
        stats.notesBefore += segment.endNote - segment.firstNote;
        ++stats.phrases;
        next = segment.endNote;
    }
    for (; next < notes.size(); ++next)
        result.push_back (notes[next]);
    notes = std::move (result);
    assignNotesToSegments (selection, notes);
    return stats;
}

void measureReach (std::vector<NoteWithKey>& notes, const PitchTrack& pitch, const ReachSettings& settings)
{
    const auto smooth = smoothedPitchMidi (pitch, settings.smoothSeconds);
    for (auto& entry : notes)
    {
        entry.reachFraction = entry.settleSeconds = -1.0;
        if (! entry.hasReferenceTarget)
            continue;
        size_t voiced = 0, within = 0;
        for (size_t f = 0; f < pitch.frames.size(); ++f)
        {
            const auto& frame = pitch.frames[f];
            if (! frame.voiced || frame.timeSeconds < entry.note.startSeconds || frame.timeSeconds > entry.note.endSeconds)
                continue;
            ++voiced;
            if (std::abs (smooth[f] - entry.nearestScaleMidi) <= settings.toleranceSemitones)
            {
                ++within;
                if (entry.settleSeconds < 0.0)
                    entry.settleSeconds = std::max (0.0, frame.timeSeconds - entry.note.startSeconds);
            }
        }
        if (voiced > 0)
            entry.reachFraction = (double) within / (double) voiced;
    }
}

std::string describeSelection (const ReferenceSelection& selection, const std::vector<ReferenceCandidate>& candidates)
{
    std::string text;
    char line[512];
    for (const auto& segment : selection.segments)
    {
        std::string matches;
        for (size_t c = 0; c < candidates.size(); ++c)
        {
            char part[200];
            char shift[32] = "";
            if (segment.shiftBeats[c] != 0)
                std::snprintf (shift, sizeof (shift), " @%+d beats", segment.shiftBeats[c]);
            std::snprintf (part, sizeof (part), "  %s%s %.0f/%.0f%% (present %.0f%%)", candidates[c].name.c_str(), shift,
                           segment.strictMatch[c] * 100.0, segment.looseMatch[c] * 100.0, segment.presence[c] * 100.0);
            matches += part;
        }
        if (segment.candidate < 0)
            std::snprintf (line, sizeof (line), "  %7.2f-%7.2fs  (none)                      |", segment.startSeconds, segment.endSeconds);
        else
        {
            const auto& candidate = candidates[(size_t) segment.candidate];
            const auto partName = segment.part >= 0 ? " [" + candidate.melody.partName (segment.part) + "]" : std::string();
            char shift[64] = "";
            if (segment.chosenShiftBeats() != 0)
                std::snprintf (shift, sizeof (shift), " %+d beats (%+.2f s; unshifted best %.0f%%)", segment.chosenShiftBeats(),
                               segment.chosenShiftBeats() * selection.beatSeconds, segment.unshiftedBestLooseMatch * 100.0);
            std::snprintf (line, sizeof (line), "  %7.2f-%7.2fs  %s%s %+doct%s%s |", segment.startSeconds, segment.endSeconds, candidate.name.c_str(),
                           partName.c_str(), segment.octave, shift, segment.matched ? "" : " (fallback)");
        }
        text += line + matches + "\n";
    }
    return text;
}

} // namespace shizuku
