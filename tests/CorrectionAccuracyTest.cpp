// Synthetic test for the pitch correction: a detuned voice-like melody (scoops, vibrato, falls) is corrected,
// the result is analysed again, and the note centres and vibrato depth are compared with the expectation.
// Also: planCorrection lets a passing note (short, or a glide) between corrected notes follow their shifts,
// and widens the change of shift across a connection for large differences; a reference-aligned part that slides
// towards its target without reaching it is moved there along the target pitch curve, keeping the vibrato; the
// processed signal is never mixed with the original while the voice sounds (processed regions, see renderCorrection).
// Exits non-zero when any check fails.

#include "../src/analysis/ReferenceSelection.h"
#include "../src/analysis/SongAnalyzer.h"
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
    if (! condition)
    {
        ++failures;
        std::printf ("  FAIL: %s\n", what);
    }
}

// Pitch (MIDI) of the voiced frames of a track inside [from, to).
std::vector<double> pitchBetween (const PitchTrack& track, double from, double to)
{
    std::vector<double> values;
    for (const auto& frame : track.frames)
        if (frame.voiced && frame.timeSeconds >= from && frame.timeSeconds < to)
            values.push_back (frequencyToMidi (frame.frequencyHz));
    return values;
}

double median (std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::nth_element (values.begin(), values.begin() + (long) values.size() / 2, values.end());
    return values[values.size() / 2];
}

// Vibrato depth as the RMS deviation (cents) from the median, over the steady vibrato part of a note.
double vibratoRmsCents (const std::vector<double>& values)
{
    const double centre = median (values);
    double sum = 0.0;
    for (double v : values)
        sum += (v - centre) * (v - centre);
    return values.empty() ? 0.0 : 100.0 * std::sqrt (sum / (double) values.size());
}

struct Summary
{
    double maxResidualErrorCents = 0.0;  // |measured corrected centre - expected corrected centre|
    double maxTargetDistanceCents = 0.0; // |measured corrected centre - scale target| over corrected notes
    double minVibratoRatio = 1.0e9, maxVibratoRatio = 0.0;
    size_t corrected = 0, kept = 0;
};

Summary runCase (const MusicalKey& key, double strengthPercent, double thresholdCents, bool verbose)
{
    const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
    const auto melody = synth::detunedMelody (key, 60, 0.5);
    const auto vocal = synth::renderVocal (melody, synth::VoiceTimbre::voiceLike, sampleRate, accompaniment.size() / sampleRate);

    const auto before = analyseSong (vocal, sampleRate, accompaniment, sampleRate);
    CorrectionSettings settings;
    settings.strength = strengthPercent / 100.0;
    settings.thresholdCents = thresholdCents;
    const auto curve = planCorrection (before, settings);
    const auto corrected = renderCorrection ({ vocal }, sampleRate, curve, 0.0, medianVoicedFrequency (before.pitch))[0];
    const auto after = PitchTracker().analyse (corrected, sampleRate);

    Summary summary;
    if (verbose)
        std::printf ("  true-cents  centre-before  target  shift  expected  measured  resid  vib-before vib-after\n");
    for (size_t i = 0; i < before.notes.size(); ++i)
    {
        const auto& note = before.notes[i].note;
        const auto& decision = curve.notes[i];
        // Same "middle 60 %" window the segmenter uses for the centre.
        const double length = note.endSeconds - note.startSeconds;
        const double centreBefore = median (pitchBetween (before.pitch, note.startSeconds + 0.2 * length, note.endSeconds - 0.2 * length));
        const double centreAfter = median (pitchBetween (after, note.startSeconds + 0.2 * length, note.endSeconds - 0.2 * length));
        const double expected = centreBefore + decision.shiftSemitones;
        const double residual = std::abs (centreAfter - expected) * 100.0;
        summary.maxResidualErrorCents = std::max (summary.maxResidualErrorCents, residual);
        if (decision.corrected)
        {
            ++summary.corrected;
            summary.maxTargetDistanceCents = std::max (summary.maxTargetDistanceCents, std::abs (centreAfter - decision.targetMidi) * 100.0);
        }
        else
        {
            ++summary.kept;
        }

        // Vibrato only on notes long enough to have a settled vibrato part (starts 150 ms in, full depth after 350 ms).
        double vibBefore = 0.0, vibAfter = 0.0;
        if (length >= 0.5)
        {
            const double from = note.startSeconds + 0.35, to = note.endSeconds - 0.08;
            vibBefore = vibratoRmsCents (pitchBetween (before.pitch, from, to));
            vibAfter = vibratoRmsCents (pitchBetween (after, from, to));
            const double ratio = vibBefore > 0.0 ? vibAfter / vibBefore : 0.0;
            summary.minVibratoRatio = std::min (summary.minVibratoRatio, ratio);
            summary.maxVibratoRatio = std::max (summary.maxVibratoRatio, ratio);
        }
        if (verbose)
        {
            double trueCents = 0.0;
            for (const auto& truth : melody)
                if (truth.startSeconds <= 0.5 * (note.startSeconds + note.endSeconds) && truth.endSeconds >= 0.5 * (note.startSeconds + note.endSeconds))
                    trueCents = (truth.midi - std::round (truth.midi)) * 100.0;
            std::printf ("  %+6.1f      %8.3f      %-5s  %+6.1f  %8.3f  %8.3f  %5.1f  %s\n", trueCents, centreBefore,
                         midiNoteName ((int) decision.targetMidi).c_str(), decision.shiftSemitones * 100.0, expected, centreAfter, residual,
                         length >= 0.5 ? (std::to_string (vibBefore).substr (0, 5) + "c     " + std::to_string (vibAfter).substr (0, 5) + "c").c_str() : "-");
        }
    }
    return summary;
}

void report (const char* name, const Summary& s)
{
    std::printf ("%-34s corrected %2zu kept %2zu | max |measured-expected| %.1fc | max |corrected-target| %.1fc | vibrato ratio %.2f..%.2f\n",
                 name, s.corrected, s.kept, s.maxResidualErrorCents, s.maxTargetDistanceCents, s.minVibratoRatio, s.maxVibratoRatio);
}
} // namespace

int main()
{
    const CorrectionSettings defaultSettings;
    check (defaultSettings.strength == 1.0 && defaultSettings.thresholdCents == 25.0, "defaults are strength 100%, threshold 25c");
    std::printf ("A minor, voice-like, strength %.0f%%, threshold %.0fc (default):\n", defaultSettings.strength * 100.0, defaultSettings.thresholdCents);
    const auto defaults = runCase ({ 9, KeyMode::minor }, defaultSettings.strength * 100.0, defaultSettings.thresholdCents, true);
    report ("A minor default / 25c", defaults);
    check (defaults.corrected > 0 && defaults.kept > 0, "default: some notes corrected, small deviations kept");
    check (defaults.maxResidualErrorCents <= 8.0, "default: corrected centre within 8 cents of expectation");
    check (defaults.maxTargetDistanceCents <= (1.0 - defaultSettings.strength) * 50.0 + 8.0,
           "default: corrected notes within (1 - strength) x 50c (+8c) of the target");

    const auto partial = runCase ({ 9, KeyMode::minor }, 80.0, 25.0, false);
    report ("A minor 80% / 25c", partial);
    check (partial.maxResidualErrorCents <= 8.0, "80%: corrected centre within 8 cents of expectation");
    check (partial.maxTargetDistanceCents <= 0.2 * 50.0 + 8.0, "80%: corrected notes within 20% of 50c (+8c) of the target");
    check (partial.minVibratoRatio >= 0.8 && partial.maxVibratoRatio <= 1.25, "80%: vibrato depth kept within -20%..+25%");
    check (defaults.minVibratoRatio >= 0.8 && defaults.maxVibratoRatio <= 1.25, "default: vibrato depth kept within -20%..+25%");

    std::printf ("\n");
    for (const auto& key : { MusicalKey { 0, KeyMode::major }, MusicalKey { 6, KeyMode::major }, MusicalKey { 4, KeyMode::minor } })
    {
        const auto full = runCase (key, 100.0, 0.0, false);
        const auto name = keyName (key) + " 100% / 0c";
        report (name.c_str(), full);
        check (full.kept == 0, "full: every note corrected");
        check (full.maxTargetDistanceCents <= 8.0, "full: corrected centres within 8 cents of the scale note");
        check (full.minVibratoRatio >= 0.8 && full.maxVibratoRatio <= 1.25, "full: vibrato depth kept within -20%..+25%");
    }

    std::printf ("\nPassing notes and connections (planCorrection only):\n");
    {
        // Three connected notes: 0.10-0.50 s (100 cents flat), a 60 ms passing note, 0.57-1.00 s (200 cents flat);
        // then a separate glide 1.5-1.7 s connected to a corrected note 1.71-2.2 s (50 cents sharp).
        SongAnalysis analysis;
        const auto add = [&] (double start, double end, double centre, int target, bool glide)
        {
            NoteWithKey entry;
            entry.note.startSeconds = start;
            entry.note.endSeconds = end;
            entry.note.centreMidi = centre;
            entry.note.glide = glide;
            entry.nearestScaleMidi = target;
            analysis.notes.push_back (entry);
        };
        add (0.10, 0.50, 69.0, 70, false);
        add (0.505, 0.565, 70.3, 71, false);
        add (0.57, 1.00, 70.0, 72, false);
        add (1.50, 1.70, 72.0, 74, true);
        add (1.71, 2.20, 74.5, 74, false);
        const auto curve = planCorrection (analysis, CorrectionSettings {});
        check (curve.notes[1].follows && std::abs (curve.notes[1].shiftSemitones - 1.5) < 0.05,
               "passing note between +1 and +2 semitones follows them (+1.5)");
        check (curve.notes[3].follows && std::abs (curve.notes[3].shiftSemitones + 0.5) < 1.0e-9, "glide follows the corrected note it leads to");
        check (std::abs (curve.shiftAt (0.3) - 1.0) < 1.0e-6 && std::abs (curve.shiftAt (0.8) - 2.0) < 1.0e-6, "note centres keep their own shift");
        double maxStep = 0.0;
        for (double t = 0.2; t < 1.0; t += 0.001)
            maxStep = std::max (maxStep, std::abs (curve.shiftAt (t + 0.001) - curve.shiftAt (t)));
        check (maxStep < 0.05, "shift changes by < 5 cents per ms across the connections");
    }

    std::printf ("\nTarget pitch curve (reference-aligned parts):\n");
    {
        // A harmonic tone with vibrato (+-30 cents, 5.8 Hz): G4 0.3-0.7 s, then a slide that only reaches 68.4
        // (60 cents short of A4) by 1.0 s and stays there to 1.3 s. Parts: G4 0.3-0.7 s, A4 0.7-1.3 s (aligned).
        const auto midiAt = [] (double t)
        {
            const double base = t < 0.7 ? 67.0 : 67.0 + 1.4 * std::min (1.0, (t - 0.7) / 0.3);
            return base + 0.3 * std::sin (2.0 * M_PI * 5.8 * t);
        };
        std::vector<float> tone ((size_t) (1.6 * sampleRate), 0.0f);
        double phase = 0.0;
        for (size_t i = 0; i < tone.size(); ++i)
        {
            const double t = (double) i / sampleRate;
            if (t < 0.3 || t >= 1.3)
                continue;
            phase += 2.0 * M_PI * midiToFrequency (midiAt (t)) / sampleRate;
            const double envelope = std::min ({ 1.0, (t - 0.3) / 0.02, (1.3 - t) / 0.02 });
            double sample = 0.0;
            for (int h = 1; h <= 8; ++h)
                sample += std::sin (h * phase) / h;
            tone[i] = (float) (0.2 * envelope * sample);
        }
        SongAnalysis analysis;
        analysis.hasVocal = true;
        analysis.pitch = PitchTracker().analyse (tone, sampleRate);
        const auto part = [&] (double from, double to, int target)
        {
            size_t first = 0, last = 0;
            for (size_t f = 0; f < analysis.pitch.frames.size(); ++f)
            {
                if (analysis.pitch.frames[f].timeSeconds < from)
                    first = f + 1;
                if (analysis.pitch.frames[f].timeSeconds < to)
                    last = f + 1;
            }
            NoteWithKey entry;
            entry.note = noteFromFrames (analysis.pitch, first, last);
            entry.nearestScaleMidi = target;
            entry.hasReferenceTarget = true;
            entry.referenceCurve = true;
            entry.alignedReferenceNote = target == 67 ? 0 : 1;
            analysis.notes.push_back (entry);
        };
        part (0.3, 0.7, 67);
        part (0.7, 1.3, 69);
        measureReach (analysis.notes, analysis.pitch);
        const auto curve = planCorrection (analysis, CorrectionSettings {});
        const auto corrected = renderCorrection ({ tone }, sampleRate, curve, 0.0, 400.0)[0];
        const auto after = PitchTracker().analyse (corrected, sampleRate);
        // Reach before / after on the A4 part, and the vibrato kept on both parts.
        auto measured = analysis.notes;
        measureReach (measured, after);
        const auto vibratoIn = [&] (const PitchTrack& track, double from, double to)
        {
            const auto smooth = smoothedPitchMidi (track, 0.16);
            double sum = 0.0;
            size_t count = 0;
            for (size_t f = 0; f < track.frames.size(); ++f)
                if (track.frames[f].voiced && track.frames[f].timeSeconds >= from && track.frames[f].timeSeconds < to)
                {
                    const double d = frequencyToMidi (track.frames[f].frequencyHz) - smooth[f];
                    sum += d * d;
                    ++count;
                }
            return count > 0 ? 100.0 * std::sqrt (sum / (double) count) : 0.0;
        };
        const double vibratoBefore = vibratoIn (analysis.pitch, 0.4, 1.2), vibratoAfter = vibratoIn (after, 0.4, 1.2);
        std::printf ("  A4 part reach: before %.0f%%, after %.0f%%; G4 part: before %.0f%%, after %.0f%%; vibrato %.1fc -> %.1fc\n",
                     analysis.notes[1].reachFraction * 100.0, measured[1].reachFraction * 100.0, analysis.notes[0].reachFraction * 100.0,
                     measured[0].reachFraction * 100.0, vibratoBefore, vibratoAfter);
        check (curve.notes[1].curve && curve.notes[1].corrected, "the aligned part is corrected along the curve");
        check (analysis.notes[1].reachFraction < 0.5 && measured[1].reachFraction >= 0.85, "the slide that falls short reaches A4 after correction");
        check (measured[0].reachFraction >= 0.9, "the held G4 stays on G4");
        check (vibratoAfter >= 0.75 * vibratoBefore && vibratoAfter <= 1.25 * vibratoBefore, "vibrato depth kept within +-25 %");
        // The shift is not constant over the slide (time-varying) and changes smoothly.
        double maxStep = 0.0;
        for (double t = 0.35; t < 1.25; t += 0.001)
            maxStep = std::max (maxStep, std::abs (curve.shiftAt (t + 0.001) - curve.shiftAt (t)));
        check (curve.shiftAt (0.8) - curve.shiftAt (1.15) > 0.4, "the shift is larger early in the slide than once it has settled");
        std::printf ("  max shift step %.1f cents per ms\n", maxStep * 100.0);
        check (maxStep < 0.06, "shift changes by < 6 cents per ms");
    }

    std::printf ("\nProcessed regions (no mix of processed and original signal while the voice sounds):\n");
    {
        // Mixing Stretch's output with the input combs (a phaser-like sound), because the output keeps a phase offset
        // to the input after a shift. Fade-outs (back to the original) only where the voice is quiet; fade-ins come
        // before the region's first shift, where the processed output still equals the input.
        const MusicalKey key { 9, KeyMode::minor };
        const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
        const auto vocal = synth::renderVocal (synth::detunedMelody (key, 60, 0.5), synth::VoiceTimbre::voiceLike, sampleRate,
                                               accompaniment.size() / sampleRate);
        const auto analysis = analyseSong (vocal, sampleRate, accompaniment, sampleRate);
        const CorrectionSettings settings;
        const auto curve = planCorrection (analysis, settings);
        const auto loudestNear = [&] (double t)
        {
            double loudest = -120.0;
            for (const auto& frame : analysis.pitch.frames)
                if (std::abs (frame.timeSeconds - t) <= analysis.pitch.hopSeconds)
                    loudest = std::max (loudest, frame.rmsDb);
            return loudest;
        };
        double fadeOut = 0.0, fadeOutLoud = 0.0;
        for (size_t p = 1; p < curve.points.size(); ++p)
            if (curve.points[p].wet < curve.points[p - 1].wet)
            {
                fadeOut += 0.001;
                fadeOutLoud += loudestNear (curve.points[p].timeSeconds) > settings.switchQuietDb ? 0.001 : 0.0;
            }
        std::printf ("  fade-outs %.3f s, of it %.3f s where the voice is above %.0f dB\n", fadeOut, fadeOutLoud, settings.switchQuietDb);
        check (fadeOut > 0.0 && fadeOutLoud == 0.0, "fade-outs to the original only where the voice is quiet");

        const auto corrected = renderCorrection ({ vocal }, sampleRate, curve, 0.0, medianVoicedFrequency (analysis.pitch))[0];
        double firstShift = -1.0, firstWet = -1.0;
        for (const auto& point : curve.points)
        {
            if (firstWet < 0.0 && point.wet > 0.0)
                firstWet = point.timeSeconds;
            if (firstShift < 0.0 && point.shiftSemitones != 0.0)
                firstShift = point.timeSeconds;
        }
        double maxDifference = 0.0;
        for (long i = (long) (firstWet * sampleRate); i < (long) ((firstShift - 0.002) * sampleRate); ++i)
            maxDifference = std::max (maxDifference, (double) std::abs (corrected[(size_t) i] - vocal[(size_t) i]));
        std::printf ("  first region: wet from %.3f s, shift from %.3f s, max |output - input| before the shift %.2g\n", firstWet, firstShift, maxDifference);
        check (firstWet >= 0.0 && firstShift > firstWet + 0.05 && maxDifference < 1.0e-4,
               "a region starts before its first shift, where the processed output still equals the input");
    }

    const auto off = runCase ({ 9, KeyMode::minor }, 0.0, 25.0, false);
    report ("A minor 0% (bypass)", off);
    check (off.corrected == 0 && off.maxResidualErrorCents <= 1.0, "strength 0: output pitch unchanged");

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
