#pragma once

#include "SongAnalyzer.h"

#include <vector>

namespace shizuku
{

struct CorrectionSettings
{
    double strength = 1.0;          // 0..1: fraction of the distance from the note centre to the target that is removed
    double thresholdCents = 25.0;   // notes whose centre is closer than this to the target are left alone
    double minNoteSeconds = 0.1;    // shorter notes are passing tones / connections (they follow their neighbours)
    double unsureFollowSeconds = 0.2; // notes marked referenceUnsure and shorter than this follow their neighbours too
    double rampSeconds = 0.02;      // smoothing of the shift (+-rampSeconds) at note edges
    double legatoGapSeconds = 0.04; // notes closer than this are treated as connected: the shift glides between them
    // Between connected notes the shift changes linearly over +-(rampSeconds + transitionSecondsPerSemitone x the
    // change), at most maxTransitionSeconds and a quarter of either note, so large differences do not switch abruptly.
    double transitionSecondsPerSemitone = 0.012;
    double maxTransitionSeconds = 0.06;

    // Reference-aligned parts (NoteWithKey::referenceCurve): the shift follows a target pitch curve instead of
    // being constant. The curve is each part's target, changing linearly over curveTransitionSeconds across the
    // boundary to a connected curve part (centred on the boundary); at a free start / end (after or before a gap,
    // or next to a part that is not on a curve) the shift fades in / out over curveEdgeSeconds, so the attack and
    // release keep a short natural movement. shift(t) = strength x (target curve - vibrato-free sung line), with
    // the sung line as in smoothedPitchMidi (curveSmoothSeconds): vibrato and small wobbles ride on top unchanged.
    // The difference to the part's constant shift is limited to +-maxCurveDeviationSemitones (pitch-track spikes).
    // A curve part is left alone when its centre is within the threshold and its vibrato-free pitch is within
    // reachToleranceSemitones of the target for at least minReachFraction of it.
    double curveTransitionSeconds = 0.04;
    double curveEdgeSeconds = 0.03;
    double curveSmoothSeconds = 0.16;
    double maxCurveDeviationSemitones = 1.5;
    double reachToleranceSemitones = 0.5;
    double minReachFraction = 0.8;

    // Processed regions (wet = 1): once Stretch has shifted, its output keeps a phase offset to the input even after
    // the shift is back at 0, so mixing the two combs (a phaser-like sound). A region therefore starts shortly
    // before the shift leaves 0 (where a fresh Stretch still outputs the input itself) and stays fully processed
    // until the first quiet point (pitch frames at or below switchQuietDb for 2 x switchFadeSeconds) after the shift
    // is back at 0, at most maxProcessedTailSeconds later (else the quietest point there); only there it fades to
    // the original over switchFadeSeconds. Shifts that resume before that quiet point join the same region.
    double switchQuietDb = -45.0;
    double switchFadeSeconds = 0.01;
    double maxProcessedTailSeconds = 1.0;
};

// What the corrector decided for one detected note.
struct NoteCorrection
{
    double targetMidi = 0.0;     // scale note the note is pulled towards
    double deviationCents = 0.0; // centreMidi - targetMidi, in cents
    double shiftSemitones = 0.0; // applied shift (0 when not corrected)
    bool corrected = false;
    // Passing note (shorter than minNoteSeconds, a glide without a stable pitch, or a short note whose reference
    // was unsure: see unsureFollowSeconds) connected to corrected
    // notes: no target of its own; it carries its neighbours' shift (interpolated between them), so a scoop or a
    // connection moves together with the notes it joins instead of jumping back to the original pitch.
    bool follows = false;
    bool edited = false; // decided by a hand edit (see NoteOverride), not by the automatic rules
    // Corrected along the target pitch curve (reference-aligned part); shiftSemitones is then the shift of the
    // note centre (strength x (target - centre)), the applied shift varies over the note.
    bool curve = false;
};

// A hand edit resolved to one detected note (see NoteEdits.h). It takes precedence over every automatic rule
// (threshold, strength, passing-note following, reference distance limits / confidence).
enum class NoteEditMode
{
    automatic, // no edit: the automatic decision
    target,    // move the note centre exactly to targetMidi (fractional MIDI, 1 cent = 0.01)
    keep       // leave the note at its sung pitch
};

struct NoteOverride
{
    NoteEditMode mode = NoteEditMode::automatic;
    double targetMidi = 0.0;
};

// Time-varying pitch shift for the vocal, in analysis time (seconds). Between breakpoints the shift and the
// wet amount are interpolated linearly; outside the breakpoints both are zero. "wet" is the amount of
// pitch-shifted signal in the output: 0 means the original samples are output unchanged (breaths,
// consonants, notes that need no correction). wet is 1 over each processed region and only fades (over
// CorrectionSettings::switchFadeSeconds) where the processed signal equals the input or the voice is quiet.
struct CorrectionCurve
{
    struct Point
    {
        double timeSeconds;
        double shiftSemitones;
        double wet;
    };
    std::vector<Point> points;           // sorted by time
    std::vector<NoteCorrection> notes;   // parallel to SongAnalysis::notes

    double shiftAt (double timeSeconds) const noexcept { return evaluate (timeSeconds).shiftSemitones; }
    double wetAt (double timeSeconds) const noexcept { return evaluate (timeSeconds).wet; }
    Point evaluate (double timeSeconds) const noexcept;
};

// Decides a constant shift per note (only the note centre moves, so vibrato, scoops and falls ride on top
// unchanged) and builds the shift curve with ramps at note edges and glides across legato connections.
// Reference-aligned parts (referenceCurve) follow the target pitch curve instead (see CorrectionSettings); they
// are never passing notes.
// Passing notes follow their connected neighbours (see NoteCorrection::follows).
// overrides: empty, or parallel to analysis.notes. Edited notes are never passing notes; their neighbours'
// passing notes follow them like any corrected (target) or uncorrected (keep) note.
CorrectionCurve planCorrection (const SongAnalysis& analysis, const CorrectionSettings& settings,
                                const std::vector<NoteOverride>& overrides = {});

// Applies the curve to multi-channel audio with Signalsmith Stretch (formants preserved; 70 ms blocks, 17.5 ms
// interval: less phasiness on voices than the 120 / 30 ms default preset). Each processed region (a run of wet > 0)
// is rendered by a freshly reset Stretch, so a region's output depends only on the input and the curve inside it.
// sampleZeroSeconds is the curve time of input sample 0. The output has the same length as the input and
// is sample-aligned with it; where the curve's wet amount is 0 the output equals the input exactly.
// fundamentalHintHz is a typical f0 of the voice used for formant estimation (0 = let Stretch estimate).
std::vector<std::vector<float>> renderCorrection (const std::vector<std::vector<float>>& input, double sampleRate,
                                                  const CorrectionCurve& curve, double sampleZeroSeconds,
                                                  double fundamentalHintHz);

struct TimeSpan
{
    double startSeconds = 0.0, endSeconds = 0.0;
};

// Where two curves of the same analysis differ (1 ms grid), each span widened outwards to the nearest time
// where both curves are dry (wet 0): there the output of either curve is the input itself, so a span can be
// re-rendered alone and spliced in without a seam. Overlapping spans are merged; sorted by time.
std::vector<TimeSpan> changedSpans (const CorrectionCurve& previous, const CorrectionCurve& next);

// Output for a hand-edited curve built on an automatic render: outside changedSpans (baseCurve, editedCurve)
// it is baseOutput unchanged (bit-identical); inside each span it is renderCorrection of the input with
// editedCurve, started fresh at the span. The result depends only on the two curves and baseOutput, not on the
// order of the edits. previous (optional, a result for the same base): its spans that are unchanged (same span,
// editedCurve equal to previousCurve inside) are copied instead of rendered again.
struct EditedRender
{
    std::vector<std::vector<float>> output;
    std::vector<TimeSpan> spans;         // changedSpans (baseCurve, editedCurve)
    std::vector<TimeSpan> renderedSpans; // the spans actually rendered this time
};

EditedRender renderEditedSpans (const std::vector<std::vector<float>>& input, double sampleRate, const CorrectionCurve& baseCurve,
                                const std::vector<std::vector<float>>& baseOutput, const CorrectionCurve& editedCurve,
                                double sampleZeroSeconds, double fundamentalHintHz, const EditedRender* previous = nullptr,
                                const CorrectionCurve* previousCurve = nullptr);

// Median voiced f0 of a pitch track (0 if nothing is voiced).
double medianVoicedFrequency (const PitchTrack& track);

// Per-frame pitch after correction (MIDI, 0 for unvoiced frames), for display and tests.
std::vector<double> correctedPitchMidi (const PitchTrack& track, const CorrectionCurve& curve);

} // namespace shizuku
