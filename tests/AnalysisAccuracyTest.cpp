// Synthetic accuracy test for the analysis code: known keys, known detuned melodies, a scoop into a held note
// (centre = the held pitch), and the accompaniment tempo.
// Exits non-zero when any check fails.

#include "../src/analysis/SongAnalyzer.h"
#include "../tools/SyntheticSignals.h"

#include <cmath>
#include <functional>
#include <cstdio>

using namespace shizuku;

namespace
{
constexpr double sampleRate = 44100.0;
constexpr double maxCentsError = 10.0; // allowed |estimated - true| centre pitch per note
int failures = 0;

void check (bool condition, const char* what)
{
    if (! condition)
    {
        ++failures;
        std::printf ("  FAIL: %s\n", what);
    }
}

struct MelodyResult
{
    size_t matched = 0;
    double maxErrorCents = 0.0;
    double meanErrorCents = 0.0;
};

// Each true note must be matched by exactly one detected note overlapping its middle.
MelodyResult compareNotes (const std::vector<synth::TrueNote>& truth, const std::vector<NoteWithKey>& detected, bool verbose)
{
    MelodyResult result;
    double errorSum = 0.0;
    for (const auto& note : truth)
    {
        const double middle = 0.5 * (note.startSeconds + note.endSeconds);
        const NoteWithKey* match = nullptr;
        int count = 0;
        for (const auto& candidate : detected)
            if (candidate.note.startSeconds <= middle && candidate.note.endSeconds >= middle)
            {
                match = &candidate;
                ++count;
            }
        if (count != 1)
        {
            if (verbose)
                std::printf ("    true note %.2fs midi %.2f: %d matches\n", note.startSeconds, note.midi, count);
            continue;
        }
        const double errorCents = std::abs (match->note.centreMidi - note.midi) * 100.0;
        if (verbose)
            std::printf ("    true %.2f-%.2fs midi %7.3f | detected %.2f-%.2fs midi %7.3f | error %5.1f cents | target %s\n",
                         note.startSeconds, note.endSeconds, note.midi, match->note.startSeconds, match->note.endSeconds,
                         match->note.centreMidi, errorCents, midiNoteName (match->nearestScaleMidi).c_str());
        ++result.matched;
        errorSum += errorCents;
        result.maxErrorCents = std::max (result.maxErrorCents, errorCents);
    }
    result.meanErrorCents = result.matched > 0 ? errorSum / result.matched : 0.0;
    return result;
}

void runKeyAndMelody (const MusicalKey& key, synth::VoiceTimbre timbre, bool verbose)
{
    const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
    const auto melody = synth::detunedMelody (key, 60, 0.5);
    const auto vocal = synth::renderVocal (melody, timbre, sampleRate, accompaniment.size() / sampleRate);
    const auto analysis = analyseSong (vocal, sampleRate, accompaniment, sampleRate);
    const auto result = compareNotes (melody, analysis.notes, verbose);

    // The scale target must be the undetuned scale note (detune never exceeds 50 cents).
    size_t correctTargets = 0;
    for (const auto& note : melody)
        for (const auto& detected : analysis.notes)
            if (detected.note.startSeconds <= 0.5 * (note.startSeconds + note.endSeconds)
                && detected.note.endSeconds >= 0.5 * (note.startSeconds + note.endSeconds)
                && detected.nearestScaleMidi == (int) std::lround (note.midi))
                ++correctTargets;

    std::printf ("%-9s %-5s key=%-9s conf=%.2f segments=%zu notes %zu/%zu detected=%zu meanErr=%.1fc maxErr=%.1fc targets %zu/%zu\n",
                 keyName (key).c_str(), timbre == synth::VoiceTimbre::sine ? "sine" : "voice", keyName (analysis.key.globalKey).c_str(),
                 analysis.key.globalConfidence, analysis.key.segments.size(), result.matched, melody.size(), analysis.notes.size(),
                 result.meanErrorCents, result.maxErrorCents, correctTargets, melody.size());

    check (analysis.key.globalKey == key, "global key");
    check (analysis.key.segments.size() == 1, "single key segment");
    check (result.matched == melody.size(), "every note matched once");
    check (analysis.notes.size() == melody.size(), "no extra notes");
    check (result.maxErrorCents <= maxCentsError, "pitch error within tolerance");
    check (correctTargets == melody.size(), "scale targets");
}

void runModulation()
{
    const MusicalKey first { 0, KeyMode::major }, second { 4, KeyMode::major }; // C major -> E major
    const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (first), synth::diatonicSection (first),
                                                            synth::diatonicSection (second), synth::diatonicSection (second) },
                                                          2.0, sampleRate);
    const auto analysis = analyseSong ({}, 0.0, accompaniment, sampleRate);
    std::printf ("modulation C major -> E major at 32.0s: global=%s\n", keyName (analysis.key.globalKey).c_str());
    for (const auto& segment : analysis.key.segments)
        std::printf ("    %.1f-%.1fs %s (%.2f)\n", segment.startSeconds, segment.endSeconds, keyName (segment.key).c_str(), segment.confidence);
    check (analysis.key.segments.size() == 2, "two key segments");
    if (analysis.key.segments.size() == 2)
    {
        check (analysis.key.segments[0].key == first && analysis.key.segments[1].key == second, "segment keys");
        check (std::abs (analysis.key.segments[0].endSeconds - 32.0) <= 4.0, "modulation boundary within 4 s");
    }
}
} // namespace

// A 0.25 s scoop from 2 semitones below into a 0.5 s held A4 (one note, no split): the centre is the held pitch.
void runScoopCentre()
{
    PitchTrack track;
    track.hopSeconds = 0.005;
    for (int f = 0; f < 150; ++f)
    {
        const double t = f * track.hopSeconds;
        const double midi = t < 0.25 ? 67.0 + 2.0 * t / 0.25 : 69.0 + 0.05 * std::sin (2.0 * M_PI * 5.5 * t);
        track.frames.push_back ({ t, midiToFrequency (midi), 0.9, -20.0, true });
    }
    NoteSegmenterSettings settings;
    settings.leapSemitones = 3.0; // keep the scoop in the same note for this test
    const auto notes = NoteSegmenter (settings).segment (track);
    std::printf ("Scoop into a held note: %zu note(s), centre %.3f, glide %d\n", notes.size(), notes.empty() ? 0.0 : notes[0].centreMidi,
                 notes.empty() ? 0 : (int) notes[0].glide);
    check (notes.size() == 1 && std::abs (notes[0].centreMidi - 69.0) < 0.1 && ! notes[0].glide, "scoop: centre at the held A4");
}

// Pitch splits (NoteSegmenter): pitch moving to another level and staying there splits; movements that come back
// or do not settle (vibrato, a scoop into the note, a fall at its end) do not.
void runPitchSplits()
{
    std::printf ("Pitch splits (synthetic pitch tracks, 5.8 ms hop):\n");
    const auto segmentOf = [] (const std::function<double (double)>& midiAt, double seconds)
    {
        PitchTrack track;
        track.hopSeconds = 0.0058;
        for (int f = 0; f * track.hopSeconds < seconds; ++f)
        {
            const double t = f * track.hopSeconds;
            track.frames.push_back ({ t, midiToFrequency (midiAt (t)), 0.9, -20.0, true });
        }
        return NoteSegmenter().segment (track);
    };
    const auto vibrato = [] (double t, double depth) { return depth * std::sin (2.0 * M_PI * 5.8 * t); };
    const auto describe = [] (const char* name, const std::vector<SungNote>& notes)
    {
        std::printf ("  %-44s %zu note(s):", name, notes.size());
        for (const auto& note : notes)
            std::printf (" [%.3f-%.3f %.2f]", note.startSeconds, note.endSeconds, note.centreMidi);
        std::printf ("\n");
    };
    // Up a semitone in 60 ms after 0.3 s, held 0.3 s (vibrato +-30 cents on both).
    const auto up = segmentOf ([&] (double t) { return (t < 0.3 ? 69.0 : t < 0.36 ? 69.0 + (t - 0.3) / 0.06 : 70.0) + vibrato (t, 0.3); }, 0.66);
    describe ("up 1 st, held (vibrato 30c)", up);
    check (up.size() == 2 && std::abs (up[0].centreMidi - 69.0) < 0.15 && std::abs (up[1].centreMidi - 70.0) < 0.15
               && std::abs (up[1].startSeconds - 0.33) < 0.04,
           "moving up a semitone and staying: split near the middle of the move");
    // Up, down, up (0.25 s each level), no gap: three notes.
    const auto upDown = segmentOf ([&] (double t) { return (t < 0.25 ? 67.0 : t < 0.5 ? 68.5 : 67.0) + vibrato (t, 0.2); }, 0.75);
    describe ("67 / 68.5 / 67, 250 ms each", upDown);
    check (upDown.size() == 3, "up and down between held levels: one note per level");
    // A slow drift of a semitone over 200 ms between two held levels.
    const auto drift = segmentOf ([&] (double t) { return t < 0.2 ? 69.0 : t < 0.4 ? 69.0 + (t - 0.2) / 0.2 : 70.0; }, 0.7);
    describe ("drift 1 st over 200 ms, held", drift);
    check (drift.size() == 2, "a slow move to a held level splits as well");
    // Not split: a wide vibrato (+-70 cents), a 2-semitone scoop of 250 ms into the note, a 3-semitone fall at the
    // end, a move of 0.6 semitone (less than levelChangeSemitones).
    const auto wide = segmentOf ([&] (double t) { return 69.0 + vibrato (t, 0.7); }, 1.0);
    describe ("vibrato +-70 cents", wide);
    check (wide.size() == 1, "wide vibrato: one note");
    // (vibrato starts on the held pitch, as sung; a vibrato cycle that stalls the scoop for ~80 ms can read as a
    // short held level at the start of the note)
    const auto scoop = segmentOf ([&] (double t) { return t < 0.25 ? 67.0 + 2.0 * t / 0.25 : 69.0 + vibrato (t - 0.25, 0.3); }, 0.75);
    describe ("scoop 2 st / 250 ms into the note", scoop);
    check (scoop.size() == 1 && std::abs (scoop[0].centreMidi - 69.0) < 0.15, "scoop into a note: one note, centre on the held pitch");
    const auto fall = segmentOf ([&] (double t) { return t < 0.4 ? 69.0 + vibrato (t, 0.3) : 69.0 - 3.0 * (t - 0.4) / 0.2; }, 0.6);
    describe ("fall 3 st / 200 ms at the end", fall);
    check (fall.size() == 1, "fall at the end of a note: one note");
    const auto small = segmentOf ([&] (double t) { return t < 0.3 ? 69.0 : 69.6; }, 0.6);
    describe ("move of 0.6 st", small);
    check (small.size() == 1, "a move smaller than levelChangeSemitones: one note");
}

void runTempo()
{
    // Clicks at 142 BPM on a quiet noise floor.
    const double bpm = 142.0;
    std::vector<float> clicks ((size_t) (sampleRate * 20.0), 0.0f);
    uint32_t seed = 1;
    for (auto& sample : clicks)
    {
        seed = seed * 1664525u + 1013904223u;
        sample = 0.01f * ((float) (seed >> 8) / 16777216.0f - 0.5f);
    }
    for (double t = 0.1; t < 19.9; t += 60.0 / bpm)
        for (int i = 0; i < 400; ++i)
            clicks[(size_t) (t * sampleRate) + (size_t) i] += 0.5f * (float) std::exp (-i / 60.0) * (float) std::sin (i * 0.7);
    const auto tempo = estimateTempo (clicks, sampleRate);
    std::printf ("Tempo of 142 BPM clicks: %.2f BPM (confidence %.2f)\n", tempo.bpm, tempo.confidence);
    check (std::abs (tempo.bpm - bpm) < 0.5, "tempo within 0.5 BPM");
}

int main()
{
    runScoopCentre();
    runPitchSplits();
    runTempo();
    std::printf ("\n");
    std::printf ("Detailed: A minor, voice-like\n");
    runKeyAndMelody ({ 9, KeyMode::minor }, synth::VoiceTimbre::voiceLike, true);
    std::printf ("\nAll 24 keys:\n");
    for (int index = 0; index < 24; ++index)
        for (auto timbre : { synth::VoiceTimbre::sine, synth::VoiceTimbre::voiceLike })
            runKeyAndMelody (MusicalKey::fromIndex (index), timbre, false);
    std::printf ("\n");
    runModulation();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
