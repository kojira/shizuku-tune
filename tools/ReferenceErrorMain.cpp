// shizuku-reference-error: pitch error (cents) of a sung part against reference melodies, before and after correction.
// Usage: shizuku-reference-error <vocal.wav> <reference.svp|.mid|.wav[@offset]> [--reference more[@offset]]...
//                                [--offset seconds] [--choose auto|N] [--after corrected.wav] [--notes]
//                                [--accompaniment inst.wav | --bpm N]
// The references are candidates chosen per sung phrase exactly as shizuku-correct does (ReferenceSelection.h),
// including whole-beat shifts when the beat is known (tempo of --accompaniment, or --bpm).
// For every sung note (>= 100 ms) whose chosen reference note covers at least half of it, the error is the
// distance of the note centre (as the segmenter computes it: middle 60 %, or the plateau of a scoop / fall) to
// the reference pitch at the phrase's octave.
// The sung notes are first aligned to the chosen reference notes (alignNotesToReferences, as shizuku-correct does), so
// every measured note is one reference note the singer aimed at.
// --after: the same notes (same time windows, same centre rule) measured in the corrected file.
// Reach: per note the fraction of voiced time whose vibrato-free pitch (smoothedPitchMidi, 160 ms) is within 50 cents
// of the correct pitch ("reach before/after" lines: over all measured note time, and the notes below half).
// Without an offset the time offset is found automatically (vocal time = reference time + offset).

#include "../src/analysis/ReferenceMelody.h"
#include "../src/analysis/ReferenceSelection.h"
#include "../src/analysis/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace shizuku;

namespace
{
// Centre of the voiced frames in [from, to) computed like the segmenter's note centre (noteCentreMidi).
double centreBetween (const PitchTrack& track, double from, double to)
{
    std::vector<double> values;
    for (const auto& frame : track.frames)
        if (frame.voiced && frame.timeSeconds >= from && frame.timeSeconds < to)
            values.push_back (frequencyToMidi (frame.frequencyHz));
    if (values.empty())
        return std::nan ("");
    bool glide = false;
    return noteCentreMidi (values, NoteSegmenterSettings {}, glide);
}

void printSummary (const char* label, std::vector<double> errors)
{
    if (errors.empty())
    {
        std::printf ("%s: no notes\n", label);
        return;
    }
    for (auto& e : errors)
        e = std::abs (e);
    std::sort (errors.begin(), errors.end());
    double sum = 0.0;
    size_t within25 = 0, within50 = 0, over100 = 0;
    for (double e : errors)
    {
        sum += e;
        within25 += e <= 25.0 ? 1 : 0;
        within50 += e <= 50.0 ? 1 : 0;
        over100 += e > 100.0 ? 1 : 0;
    }
    const auto n = errors.size();
    std::printf ("%s: %zu notes | |error| median %.1fc mean %.1fc p90 %.1fc | <=25c %.0f%% <=50c %.0f%% >100c %.0f%%\n", label, n,
                 errors[n / 2], sum / (double) n, errors[(n * 9) / 10], 100.0 * (double) within25 / (double) n,
                 100.0 * (double) within50 / (double) n, 100.0 * (double) over100 / (double) n);
}

// Reach: how much of a note's voiced time the vibrato-free pitch is within 50 cents of the correct pitch.
struct Reach
{
    size_t voiced = 0, within = 0, notes = 0, notesBelowHalf = 0;
    double fraction() const { return voiced > 0 ? (double) within / (double) voiced : 0.0; }
    void add (const Reach& note)
    {
        voiced += note.voiced;
        within += note.within;
        ++notes;
        notesBelowHalf += note.fraction() < 0.5 ? 1 : 0;
    }
    void print (const char* label) const
    {
        std::printf ("%s: %zu notes | %.0f%% of the note time within 50c | %zu notes (%.0f%%) below half\n", label, notes,
                     fraction() * 100.0, notesBelowHalf, notes > 0 ? 100.0 * (double) notesBelowHalf / (double) notes : 0.0);
    }
};

Reach reachOf (const PitchTrack& track, const std::vector<double>& smooth, const SungNote& note, double correctMidi)
{
    Reach reach;
    for (size_t f = 0; f < track.frames.size(); ++f)
        if (track.frames[f].voiced && track.frames[f].timeSeconds >= note.startSeconds && track.frames[f].timeSeconds < note.endSeconds)
        {
            ++reach.voiced;
            reach.within += std::abs (smooth[f] - correctMidi) <= 0.5 ? 1 : 0;
        }
    return reach;
}

struct ReferenceArgument
{
    std::string path;
    bool offsetGiven = false;
    double offset = 0.0;
};

ReferenceArgument parseReference (std::string text)
{
    ReferenceArgument argument;
    const auto at = text.find_last_of ('@');
    if (at != std::string::npos)
    {
        argument.offset = std::atof (text.c_str() + at + 1);
        argument.offsetGiven = true;
        text.resize (at);
    }
    argument.path = text;
    return argument;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0]
                  << " <vocal.wav> <reference[@offset]> [--reference more[@offset]]... [--offset seconds] [--choose auto|N]"
                     " [--after corrected.wav] [--notes] [--accompaniment inst.wav | --bpm N]\n";
        return 2;
    }
    bool listNotes = false;
    std::vector<ReferenceArgument> references { parseReference (argv[2]) };
    std::string afterPath;
    int choose = -2;
    double bpm = 0.0;
    std::string accompanimentPath;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--offset") == 0 && i + 1 < argc)
        {
            references.back().offset = std::atof (argv[++i]);
            references.back().offsetGiven = true;
        }
        else if (std::strcmp (argv[i], "--reference") == 0 && i + 1 < argc)
            references.push_back (parseReference (argv[++i]));
        else if (std::strcmp (argv[i], "--after") == 0 && i + 1 < argc)
            afterPath = argv[++i];
        else if (std::strcmp (argv[i], "--choose") == 0 && i + 1 < argc)
        {
            ++i;
            choose = std::strcmp (argv[i], "auto") == 0 ? -2 : std::atoi (argv[i]);
        }
        else if (std::strcmp (argv[i], "--bpm") == 0 && i + 1 < argc)
            bpm = std::atof (argv[++i]);
        else if (std::strcmp (argv[i], "--accompaniment") == 0 && i + 1 < argc)
            accompanimentPath = argv[++i];
        else if (std::strcmp (argv[i], "--notes") == 0)
            listNotes = true;
        else
        {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    try
    {
        const auto vocal = readWav (argv[1]);
        auto analysis = analyseSong (vocal.mixToMono(), vocal.sampleRate, {}, 0.0);
        std::vector<ReferenceCandidate> candidates;
        for (const auto& argument : references)
        {
            ReferenceCandidate candidate;
            candidate.melody = loadReference (argument.path);
            candidate.name = candidate.melody.sourceName;
            candidate.offsetSeconds = argument.offsetGiven ? argument.offset : alignReference (analysis.notes, candidate.melody).offsetSeconds;
            candidates.push_back (std::move (candidate));
        }
        ReferenceSelectionSettings selectionSettings;
        if (! accompanimentPath.empty())
        {
            const auto accompaniment = readWav (accompanimentPath);
            const auto tempo = estimateTempo (accompaniment.mixToMono(), accompaniment.sampleRate);
            selectionSettings.beatSeconds = beatSecondsForTempo (tempo.bpm, tempo.confidence);
        }
        else if (bpm > 0.0)
            selectionSettings.beatSeconds = 60.0 / bpm;
        auto selection = selectReferences (analysis.notes, candidates, selectionSettings, choose);
        alignNotesToReferences (analysis, candidates, selection);
        const auto correct = referencePitchForErrors (analysis.notes, candidates, selection);

        PitchTrack after;
        if (! afterPath.empty())
        {
            const auto corrected = readWav (afterPath);
            after = PitchTracker().analyse (corrected.mixToMono(), corrected.sampleRate);
        }

        const auto smoothBefore = smoothedPitchMidi (analysis.pitch, 0.16);
        const auto smoothAfter = afterPath.empty() ? std::vector<double> {} : smoothedPitchMidi (after, 0.16);
        std::vector<double> before, afterErrors;
        Reach reachBefore, reachAfter;
        for (size_t i = 0; i < analysis.notes.size(); ++i)
        {
            const auto& note = analysis.notes[i].note;
            const double length = note.endSeconds - note.startSeconds;
            if (std::isnan (correct[i]) || length < 0.1)
                continue;
            const double errorBefore = (note.centreMidi - correct[i]) * 100.0;
            double errorAfter = std::nan ("");
            Reach noteAfter;
            if (! afterPath.empty())
            {
                const double centreAfter = centreBetween (after, note.startSeconds, note.endSeconds);
                if (std::isnan (centreAfter))
                    continue; // not measurable after (should not happen); keep both lists on the same notes
                errorAfter = (centreAfter - correct[i]) * 100.0;
                afterErrors.push_back (errorAfter);
                noteAfter = reachOf (after, smoothAfter, note, correct[i]);
                reachAfter.add (noteAfter);
            }
            const auto noteBefore = reachOf (analysis.pitch, smoothBefore, note, correct[i]);
            reachBefore.add (noteBefore);
            before.push_back (errorBefore);
            if (listNotes)
                std::printf ("  %7.3f-%7.3fs sung %7.3f ref %-4s error %+7.1fc reach %3.0f%%%s\n", note.startSeconds, note.endSeconds, note.centreMidi,
                             midiNoteName ((int) correct[i]).c_str(), errorBefore, noteBefore.fraction() * 100.0,
                             afterPath.empty() ? "" : ("  after " + std::to_string ((int) std::lround (errorAfter)) + "c reach "
                                                       + std::to_string ((int) std::lround (noteAfter.fraction() * 100.0)) + "%").c_str());
        }

        std::string names;
        for (const auto& candidate : candidates)
        {
            char text[160];
            std::snprintf (text, sizeof (text), "%s%s@%+.3f", names.empty() ? "" : ", ", candidate.name.c_str(), candidate.offsetSeconds);
            names += text;
        }
        std::printf ("%s vs [%s]\n", argv[1], names.c_str());
        printSummary ("before", before);
        if (! afterPath.empty())
            printSummary ("after ", afterErrors);
        reachBefore.print ("reach before");
        if (! afterPath.empty())
            reachAfter.print ("reach after ");
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
