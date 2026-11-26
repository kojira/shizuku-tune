#include "PitchCorrection.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace shizuku
{

namespace
{
constexpr double curveGridSeconds = 0.001;
// Stretch block / interval (see renderCorrection); a processed region starts this long before the shift leaves 0
// (plus the fade), so the blocks that see the first shift do not reach back into the fade-in.
constexpr double stretchBlockSeconds = 0.07;
constexpr double stretchIntervalSeconds = 0.0175;

// Centred moving average with a window of (2 * radius + 1) samples.
std::vector<double> boxSmooth (const std::vector<double>& values, int radius)
{
    if (radius <= 0 || values.empty())
        return values;
    std::vector<double> prefix (values.size() + 1, 0.0);
    for (size_t i = 0; i < values.size(); ++i)
        prefix[i + 1] = prefix[i] + values[i];
    std::vector<double> out (values.size());
    const auto n = (long) values.size();
    for (long i = 0; i < n; ++i)
    {
        const long lo = i - radius, hi = i + radius; // outside the range counts as 0
        const long clampedLo = std::max (0L, lo), clampedHi = std::min (n - 1, hi);
        out[(size_t) i] = (prefix[(size_t) clampedHi + 1] - prefix[(size_t) clampedLo]) / (double) (2 * radius + 1);
    }
    return out;
}
} // namespace

CorrectionCurve::Point CorrectionCurve::evaluate (double timeSeconds) const noexcept
{
    if (points.empty() || timeSeconds <= points.front().timeSeconds || timeSeconds >= points.back().timeSeconds)
        return { timeSeconds, 0.0, 0.0 };
    const auto upper = std::upper_bound (points.begin(), points.end(), timeSeconds,
                                         [] (double t, const Point& p) { return t < p.timeSeconds; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    const double span = b.timeSeconds - a.timeSeconds;
    const double fraction = span > 0.0 ? (timeSeconds - a.timeSeconds) / span : 0.0;
    return { timeSeconds, a.shiftSemitones + (b.shiftSemitones - a.shiftSemitones) * fraction, a.wet + (b.wet - a.wet) * fraction };
}

CorrectionCurve planCorrection (const SongAnalysis& analysis, const CorrectionSettings& settings,
                                const std::vector<NoteOverride>& overrides)
{
    CorrectionCurve curve;
    const auto& notes = analysis.notes;
    curve.notes.resize (notes.size());
    if (notes.empty())
        return curve;

    const double strength = std::clamp (settings.strength, 0.0, 1.0);
    const auto overrideFor = [&] (size_t i) { return i < overrides.size() ? overrides[i] : NoteOverride {}; };
    const auto isPassing = [&] (size_t i)
    {
        if (overrideFor (i).mode != NoteEditMode::automatic)
            return false;
        const double length = notes[i].note.endSeconds - notes[i].note.startSeconds;
        if (notes[i].referenceCurve)
            return false;
        return notes[i].note.glide || length < settings.minNoteSeconds || (notes[i].referenceUnsure && length < settings.unsureFollowSeconds);
    };
    // Vibrato-free sung line at time t (interpolated between voiced frames; 0 when no voiced frame is near).
    const auto smooth = smoothedPitchMidi (analysis.pitch, settings.curveSmoothSeconds);
    const auto& frames = analysis.pitch.frames;
    const double hop = analysis.pitch.hopSeconds;
    const auto smoothAt = [&] (double t)
    {
        if (frames.empty() || hop <= 0.0)
            return 0.0;
        const double position = (t - frames.front().timeSeconds) / hop;
        const long a = (long) std::floor (position), b = a + 1;
        const auto value = [&] (long f) { return f >= 0 && f < (long) frames.size() && frames[(size_t) f].voiced ? smooth[(size_t) f] : 0.0; };
        const double va = value (a), vb = value (b);
        if (va > 0.0 && vb > 0.0)
            return va + (vb - va) * (position - (double) a);
        if (va > 0.0 || vb > 0.0)
            return va > 0.0 ? va : vb;
        for (long d = 2; d <= 4; ++d) // short unvoiced stretch: nearest voiced frame
        {
            if (value (a - d + 1) > 0.0)
                return value (a - d + 1);
            if (value (b + d - 1) > 0.0)
                return value (b + d - 1);
        }
        return 0.0;
    };
    const auto connected = [&] (size_t i) // notes i and i + 1
    { return notes[i + 1].note.startSeconds - notes[i].note.endSeconds < settings.legatoGapSeconds; };

    // What the automatic rules decide for each note, before hand edits. Passing notes follow these for edited
    // neighbours, so a hand edit moves only the edited note (not the slide into it).
    std::vector<double> automaticShift (notes.size(), 0.0);
    std::vector<char> automaticCorrected (notes.size(), 0);
    bool anyCorrected = false;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto& decision = curve.notes[i];
        decision.targetMidi = notes[i].nearestScaleMidi;
        decision.deviationCents = (notes[i].note.centreMidi - decision.targetMidi) * 100.0;
        decision.corrected = strength > 0.0 && ! isPassing (i) && std::abs (decision.deviationCents) >= settings.thresholdCents;
        decision.shiftSemitones = decision.corrected ? -strength * decision.deviationCents / 100.0 : 0.0;
        if (notes[i].referenceCurve && strength > 0.0 && ! decision.corrected
            && notes[i].reachFraction >= 0.0 && notes[i].reachFraction < settings.minReachFraction)
        {
            automaticCorrected[i] = 1;
            automaticShift[i] = -strength * decision.deviationCents / 100.0;
        }
        else
        {
            automaticCorrected[i] = decision.corrected ? 1 : 0;
            automaticShift[i] = decision.shiftSemitones;
        }
        const auto edit = overrideFor (i);
        if (edit.mode == NoteEditMode::target)
        {
            decision.targetMidi = edit.targetMidi;
            decision.deviationCents = (notes[i].note.centreMidi - edit.targetMidi) * 100.0;
            decision.shiftSemitones = edit.targetMidi - notes[i].note.centreMidi;
            decision.corrected = true;
        }
        else if (edit.mode == NoteEditMode::keep)
        {
            decision.corrected = false;
            decision.shiftSemitones = 0.0;
        }
        if (notes[i].referenceCurve && edit.mode == NoteEditMode::automatic && strength > 0.0 && ! decision.corrected
            && notes[i].reachFraction >= 0.0 && notes[i].reachFraction < settings.minReachFraction)
        {
            decision.corrected = true; // centre close enough, but the pitch does not get there for long enough
            decision.shiftSemitones = -strength * decision.deviationCents / 100.0;
        }
        decision.curve = notes[i].referenceCurve && decision.corrected;
        decision.edited = edit.mode != NoteEditMode::automatic;
        anyCorrected = anyCorrected || decision.corrected;
    }
    if (! anyCorrected)
        return curve;

    // Passing notes take the shift of the nearest non-passing notes they are connected to (through other
    // passing notes), interpolated by time between the two sides; nothing to follow when neither side is corrected.
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (! isPassing (i))
            continue;
        long left = (long) i - 1, right = (long) i + 1;
        while (left >= 0 && connected ((size_t) left) && isPassing ((size_t) left))
            --left;
        while (right < (long) notes.size() && connected ((size_t) right - 1) && isPassing ((size_t) right))
            ++right;
        const bool hasLeft = left >= 0 && connected ((size_t) left);
        const bool hasRight = right < (long) notes.size() && connected ((size_t) right - 1);
        // An edited neighbour counts with its automatic decision (the edit belongs to that note only).
        const auto neighbourCorrected = [&] (size_t n) { return curve.notes[n].edited ? automaticCorrected[n] != 0 : curve.notes[n].corrected; };
        const auto neighbourShift = [&] (size_t n) { return curve.notes[n].edited ? automaticShift[n] : curve.notes[n].shiftSemitones; };
        const bool leftCorrected = hasLeft && neighbourCorrected ((size_t) left);
        const bool rightCorrected = hasRight && neighbourCorrected ((size_t) right);
        if (! leftCorrected && ! rightCorrected)
            continue;
        const double leftShift = hasLeft ? neighbourShift ((size_t) left) : neighbourShift ((size_t) right);
        const double rightShift = hasRight ? neighbourShift ((size_t) right) : leftShift;
        double fraction = 0.5;
        if (hasLeft && hasRight)
        {
            const double from = notes[(size_t) left].note.endSeconds, to = notes[(size_t) right].note.startSeconds;
            const double middle = 0.5 * (notes[i].note.startSeconds + notes[i].note.endSeconds);
            fraction = to > from ? std::clamp ((middle - from) / (to - from), 0.0, 1.0) : 0.5;
        }
        auto& decision = curve.notes[i];
        decision.follows = true;
        decision.corrected = true;
        decision.shiftSemitones = leftShift + (rightShift - leftShift) * fraction;
    }

    // Sample the step function "shift of the note under t" on a 1 ms grid; connected notes (legato) glide from
    // one shift to the next across their boundary.
    const double ramp = std::max (settings.rampSeconds, curveGridSeconds);
    const double fadeSeconds = std::max (settings.switchFadeSeconds, curveGridSeconds);
    const double leadSeconds = stretchBlockSeconds + fadeSeconds;
    const double origin = std::max (0.0, notes.front().note.startSeconds - 4.0 * ramp - leadSeconds);
    const double end = notes.back().note.endSeconds + 4.0 * ramp + std::max (0.0, settings.maxProcessedTailSeconds) + 3.0 * fadeSeconds;
    const auto gridSize = (size_t) std::ceil ((end - origin) / curveGridSeconds) + 1;
    const auto indexFor = [&] (double t) { return (long) std::lround ((t - origin) / curveGridSeconds); };

    std::vector<double> shift (gridSize, 0.0), active (gridSize, 0.0);
    const auto fill = [&] (double from, double to, double fromValue, double toValue, bool markActive)
    {
        const long a = std::max (0L, indexFor (from)), b = std::min ((long) gridSize - 1, indexFor (to));
        for (long i = a; i <= b; ++i)
        {
            const double fraction = b > a ? (double) (i - a) / (double) (b - a) : 0.0;
            shift[(size_t) i] = fromValue + (toValue - fromValue) * fraction;
            if (markActive)
                active[(size_t) i] = 1.0;
        }
    };
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i].note;
        const double value = curve.notes[i].shiftSemitones;
        fill (note.startSeconds, note.endSeconds, value, value, curve.notes[i].corrected);
    }
    // Target pitch curve parts: strength x (target curve - vibrato-free sung line), see CorrectionSettings.
    const auto curveTarget = [&] (size_t i)
    {
        const auto& decision = curve.notes[i];
        return notes[i].note.centreMidi + decision.shiftSemitones / (decision.edited ? 1.0 : strength);
    };
    const auto curvePair = [&] (size_t i) { return connected (i) && curve.notes[i].curve && curve.notes[i + 1].curve; };
    const double halfTransition = 0.5 * settings.curveTransitionSeconds;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& decision = curve.notes[i];
        if (! decision.curve)
            continue;
        const auto& note = notes[i].note;
        const double target = curveTarget (i);
        const double amount = decision.edited ? 1.0 : strength;
        const bool joinedBefore = i > 0 && curvePair (i - 1), joinedAfter = i + 1 < notes.size() && curvePair (i);
        const double from = joinedBefore ? 0.5 * (notes[i - 1].note.endSeconds + note.startSeconds) : note.startSeconds;
        const double to = joinedAfter ? 0.5 * (note.endSeconds + notes[i + 1].note.startSeconds) : note.endSeconds;
        const long a = std::max (0L, indexFor (from)), b = std::min ((long) gridSize - 1, indexFor (to));
        for (long g = a; g <= b; ++g)
        {
            const double t = origin + (double) g * curveGridSeconds;
            // Target curve: linear change over the transition around a boundary to a connected curve part.
            double desired = target;
            if (joinedBefore && t < from + halfTransition)
            {
                const double previous = curveTarget (i - 1);
                desired = previous + (target - previous) * std::clamp ((t - (from - halfTransition)) / settings.curveTransitionSeconds, 0.0, 1.0);
            }
            if (joinedAfter && t > to - halfTransition)
            {
                const double following = curveTarget (i + 1);
                desired = target + (following - target) * std::clamp ((t - (to - halfTransition)) / settings.curveTransitionSeconds, 0.0, 1.0);
            }
            const double sung = smoothAt (t);
            double value = decision.shiftSemitones;
            if (sung > 0.0)
                value = std::clamp (amount * (desired - sung), decision.shiftSemitones - settings.maxCurveDeviationSemitones,
                                    decision.shiftSemitones + settings.maxCurveDeviationSemitones);
            // Free edges: fade from the constant shift (the attack / release keeps its movement) to the curve.
            double weight = 1.0;
            if (! joinedBefore && settings.curveEdgeSeconds > 0.0)
                weight = std::min (weight, std::clamp ((t - note.startSeconds) / settings.curveEdgeSeconds, 0.0, 1.0));
            if (! joinedAfter && settings.curveEdgeSeconds > 0.0)
                weight = std::min (weight, std::clamp ((note.endSeconds - t) / settings.curveEdgeSeconds, 0.0, 1.0));
            shift[(size_t) g] = decision.shiftSemitones + (value - decision.shiftSemitones) * weight;
            active[(size_t) g] = 1.0;
        }
    }
    // Connections: a linear change of the shift across the boundary, wider for larger changes (between two curve
    // parts the target curve already changes over the boundary).
    for (size_t i = 0; i + 1 < notes.size(); ++i)
    {
        const auto& note = notes[i].note;
        const auto& next = notes[i + 1].note;
        const bool eitherCorrected = curve.notes[i].corrected || curve.notes[i + 1].corrected;
        if (! connected (i) || ! eitherCorrected || curvePair (i))
            continue;
        const double from = curve.notes[i].shiftSemitones, to = curve.notes[i + 1].shiftSemitones;
        const double halfWidth = std::min ({ settings.rampSeconds + settings.transitionSecondsPerSemitone * std::abs (to - from),
                                             settings.maxTransitionSeconds, 0.25 * (note.endSeconds - note.startSeconds),
                                             0.25 * (next.endSeconds - next.startSeconds) });
        fill (note.endSeconds - halfWidth, next.startSeconds + halfWidth, from, to, true);
    }

    // Ramps: smooth the shift over +-ramp.
    const int radius = (int) std::lround (ramp / curveGridSeconds);
    const auto smoothShift = boxSmooth (shift, radius);

    // Processed regions (see CorrectionSettings::switchQuietDb): wet 1 from leadSeconds before the shift leaves 0
    // (where the output of a fresh Stretch is still the input) to a quiet point after it is back at 0, then a short
    // fade to the original. Never a mix of processed and original signal while the voice sounds.
    const auto frameDb = [&] (double t)
    {
        if (frames.empty() || hop <= 0.0)
            return -120.0;
        const long f = (long) std::lround ((t - frames.front().timeSeconds) / hop);
        return f >= 0 && f < (long) frames.size() ? frames[(size_t) f].rmsDb : -120.0;
    };
    const long fadeSteps = std::max (1L, (long) std::lround (fadeSeconds / curveGridSeconds));
    const long leadSteps = (long) std::lround (leadSeconds / curveGridSeconds);
    const long tailSteps = (long) std::lround (std::max (0.0, settings.maxProcessedTailSeconds) / curveGridSeconds);
    const auto loudestOver = [&] (long g) // loudest pitch frame within [g, g + 2 x fade]
    {
        double loudest = -120.0;
        for (long k = g; k <= g + 2 * fadeSteps; k += std::max (1L, (long) std::floor (hop / curveGridSeconds)))
            loudest = std::max (loudest, frameDb (origin + (double) k * curveGridSeconds));
        return std::max (loudest, frameDb (origin + (double) (g + 2 * fadeSteps) * curveGridSeconds));
    };
    // Start of the fade out after the shift is back at 0 at grid index from.
    const auto switchPointFrom = [&] (long from)
    {
        long quietest = from;
        double quietestDb = 1.0e9;
        for (long g = from; g <= from + tailSteps; ++g)
        {
            const double db = loudestOver (g);
            if (db <= settings.switchQuietDb)
                return g;
            if (db < quietestDb)
            {
                quietestDb = db;
                quietest = g;
            }
        }
        return quietest;
    };
    std::vector<char> needed (gridSize, 0);
    for (size_t i = 0; i < gridSize; ++i)
        needed[i] = active[i] > 0.0 || smoothShift[i] != 0.0 ? 1 : 0;
    std::vector<double> wet (gridSize, 0.0);
    for (long g = 0; g < (long) gridSize;)
    {
        if (! needed[(size_t) g])
        {
            ++g;
            continue;
        }
        const long begin = std::max (0L, g - leadSteps);
        long switchAt = g;
        for (;;) // shifts that resume before the switch point join the region
        {
            long last = g;
            while (last + 1 < (long) gridSize && needed[(size_t) last + 1])
                ++last;
            switchAt = switchPointFrom (last + 1);
            long next = last + 1;
            while (next < (long) gridSize && ! needed[(size_t) next])
                ++next;
            if (next < (long) gridSize && next - leadSteps <= switchAt + fadeSteps)
            {
                g = next;
                continue;
            }
            g = next;
            break;
        }
        for (long k = begin; k <= std::min ((long) gridSize - 1, switchAt + fadeSteps); ++k)
        {
            const double fadeIn = std::min (1.0, (double) (k - begin + 1) / (double) fadeSteps);
            const double fadeOut = std::clamp ((double) (switchAt + fadeSteps - k) / (double) fadeSteps, 0.0, 1.0);
            wet[(size_t) k] = std::max (wet[(size_t) k], std::min (fadeIn, fadeOut));
        }
    }

    // Keep only the parts where something happens (plus one zero point on each side).
    for (size_t i = 0; i < gridSize; ++i)
    {
        const bool here = wet[i] > 0.0 || smoothShift[i] != 0.0;
        const bool neighbour = (i > 0 && (wet[i - 1] > 0.0 || smoothShift[i - 1] != 0.0))
                               || (i + 1 < gridSize && (wet[i + 1] > 0.0 || smoothShift[i + 1] != 0.0));
        if (here || neighbour)
            curve.points.push_back ({ origin + (double) i * curveGridSeconds, wet[i] > 0.0 ? smoothShift[i] : 0.0, wet[i] });
    }
    return curve;
}

namespace
{
// Renders [firstNeeded, lastNeeded) of the input with the curve into output (one processed region).
void renderRegion (const std::vector<std::vector<float>>& input, std::vector<std::vector<float>>& output, double sampleRate,
                   const CorrectionCurve& curve, double sampleZeroSeconds, double fundamentalHintHz, long firstNeeded, long lastNeeded)
{
    const int numChannels = (int) input.size();
    const long length = (long) input[0].size();

    // A fresh Stretch per region: its output at shift 0 is the input itself until the first shift.
    signalsmith::stretch::SignalsmithStretch<float> stretch (12345);
    stretch.configure (numChannels, (int) std::lround (stretchBlockSeconds * sampleRate), (int) std::lround (stretchIntervalSeconds * sampleRate));
    stretch.setFormantBase (fundamentalHintHz > 0.0 ? (float) (fundamentalHintHz / sampleRate) : 0.0f);

    const long inputLatency = stretch.inputLatency();
    const long outputLatency = stretch.outputLatency();
    const long totalLatency = inputLatency + outputLatency;
    const long context = (long) stretch.blockSamples() + (long) stretch.intervalSamples();
    const long start = std::max (0L, firstNeeded - context);
    const long stop = std::min (length, lastNeeded + context);

    // Input index i is read at processing step i + totalLatency of the output; feed zeros past the end.
    constexpr long blockSize = 64;
    std::vector<std::vector<float>> inBlock ((size_t) numChannels, std::vector<float> (blockSize)),
        outBlock ((size_t) numChannels, std::vector<float> (blockSize));
    std::vector<float*> inPointers, outPointers;
    for (int c = 0; c < numChannels; ++c)
    {
        inPointers.push_back (inBlock[(size_t) c].data());
        outPointers.push_back (outBlock[(size_t) c].data());
    }

    stretch.reset();
    for (long position = start; position < stop + totalLatency; position += blockSize)
    {
        const long count = blockSize;
        for (int c = 0; c < numChannels; ++c)
            for (long i = 0; i < count; ++i)
            {
                const long index = position + i;
                inBlock[(size_t) c][(size_t) i] = index < stop ? input[(size_t) c][(size_t) index] : 0.0f;
            }

        // Processing time is inputLatency behind the newest input sample.
        const double processingSeconds = sampleZeroSeconds + (double) (position + count / 2 - inputLatency) / sampleRate;
        stretch.setTransposeSemitones ((float) curve.shiftAt (processingSeconds));
        stretch.setFormantFactor (1.0f, true); // keep the formants where they were
        stretch.process (inPointers.data(), (int) count, outPointers.data(), (int) count);

        for (long i = 0; i < count; ++i)
        {
            const long target = position + i - totalLatency;
            if (target < firstNeeded || target >= lastNeeded)
                continue;
            const double wet = curve.wetAt (sampleZeroSeconds + (double) target / sampleRate);
            if (wet <= 0.0)
                continue;
            for (int c = 0; c < numChannels; ++c)
            {
                const float dry = input[(size_t) c][(size_t) target];
                output[(size_t) c][(size_t) target] = (float) (dry + (outBlock[(size_t) c][(size_t) i] - dry) * wet);
            }
        }
    }
}
} // namespace

std::vector<std::vector<float>> renderCorrection (const std::vector<std::vector<float>>& input, double sampleRate,
                                                  const CorrectionCurve& curve, double sampleZeroSeconds,
                                                  double fundamentalHintHz)
{
    auto output = input;
    if (input.empty() || input[0].empty() || curve.points.empty() || sampleRate <= 0.0)
        return output;

    const long length = (long) input[0].size();

    // Only process the stretch of input the curve touches (plus context for the STFT).
    const long firstNeeded = std::max (0L, (long) std::floor ((curve.points.front().timeSeconds - sampleZeroSeconds) * sampleRate));
    const long lastNeeded = std::min (length, (long) std::ceil ((curve.points.back().timeSeconds - sampleZeroSeconds) * sampleRate) + 1);
    if (firstNeeded >= lastNeeded)
        return output;

    // Processed regions: maximal runs of points with wet > 0, from the zero point before to the zero point after.
    std::vector<std::pair<long, long>> regions; // sample ranges [first, last)
    for (size_t p = 0; p < curve.points.size();)
    {
        if (curve.points[p].wet <= 0.0)
        {
            ++p;
            continue;
        }
        size_t q = p;
        while (q + 1 < curve.points.size() && curve.points[q + 1].wet > 0.0)
            ++q;
        const double from = curve.points[p > 0 ? p - 1 : p].timeSeconds;
        const double to = curve.points[std::min (q + 1, curve.points.size() - 1)].timeSeconds;
        const long first = std::max (firstNeeded, (long) std::floor ((from - sampleZeroSeconds) * sampleRate));
        const long last = std::min (lastNeeded, (long) std::ceil ((to - sampleZeroSeconds) * sampleRate) + 1);
        if (first < last)
            regions.emplace_back (first, last);
        p = q + 1;
    }
    for (const auto& [regionFirst, regionLast] : regions)
        renderRegion (input, output, sampleRate, curve, sampleZeroSeconds, fundamentalHintHz, regionFirst, regionLast);
    return output;
}

std::vector<TimeSpan> changedSpans (const CorrectionCurve& previous, const CorrectionCurve& next)
{
    std::vector<TimeSpan> spans;
    const auto bounds = [] (const CorrectionCurve& curve, double& from, double& to)
    {
        if (curve.points.empty())
            return;
        from = std::min (from, curve.points.front().timeSeconds);
        to = std::max (to, curve.points.back().timeSeconds);
    };
    double from = 1.0e300, to = -1.0e300;
    bounds (previous, from, to);
    bounds (next, from, to);
    if (from > to)
        return spans;
    const auto count = (long) std::ceil ((to - from) / curveGridSeconds) + 1;
    const auto timeAt = [&] (long i) { return from + (double) i * curveGridSeconds; };
    const auto differs = [&] (long i)
    {
        const auto a = previous.evaluate (timeAt (i)), b = next.evaluate (timeAt (i));
        return std::abs (a.shiftSemitones - b.shiftSemitones) > 1.0e-9 || std::abs (a.wet - b.wet) > 1.0e-9;
    };
    const auto dry = [&] (long i) { return previous.wetAt (timeAt (i)) <= 0.0 && next.wetAt (timeAt (i)) <= 0.0; };
    for (long i = 0; i < count; ++i)
    {
        if (! differs (i))
            continue;
        long first = i, last = i;
        while (last + 1 < count && differs (last + 1))
            ++last;
        while (first > 0 && ! dry (first))
            --first;
        while (last + 1 < count && ! dry (last))
            ++last;
        // the grid ends are outside every curve's points: dry
        const TimeSpan span { timeAt (first), timeAt (last) };
        if (! spans.empty() && span.startSeconds <= spans.back().endSeconds)
            spans.back().endSeconds = std::max (spans.back().endSeconds, span.endSeconds);
        else
            spans.push_back (span);
        i = last;
    }
    return spans;
}

EditedRender renderEditedSpans (const std::vector<std::vector<float>>& input, double sampleRate, const CorrectionCurve& baseCurve,
                                const std::vector<std::vector<float>>& baseOutput, const CorrectionCurve& editedCurve,
                                double sampleZeroSeconds, double fundamentalHintHz, const EditedRender* previous,
                                const CorrectionCurve* previousCurve)
{
    EditedRender result;
    result.output = baseOutput;
    result.spans = changedSpans (baseCurve, editedCurve);
    if (input.empty() || baseOutput.size() != input.size() || baseOutput[0].size() != input[0].size())
        throw std::runtime_error ("renderEditedSpans: base output does not match the input");
    const long length = (long) input[0].size();
    const auto sampleRange = [&] (const TimeSpan& span)
    {
        const long first = std::clamp ((long) std::floor ((span.startSeconds - sampleZeroSeconds) * sampleRate), 0L, length);
        const long last = std::clamp ((long) std::ceil ((span.endSeconds - sampleZeroSeconds) * sampleRate) + 1, first, length);
        return std::make_pair (first, last);
    };
    const bool canReuse = previous != nullptr && previousCurve != nullptr && previous->output.size() == input.size()
                          && previous->output[0].size() == input[0].size();
    const auto sinceLast = canReuse ? changedSpans (*previousCurve, editedCurve) : std::vector<TimeSpan> {};
    for (const auto& span : result.spans)
    {
        const auto [first, last] = sampleRange (span);
        bool reused = false;
        if (canReuse)
        {
            const bool sameSpan = std::any_of (previous->spans.begin(), previous->spans.end(), [&] (const TimeSpan& old)
                                               { return std::abs (old.startSeconds - span.startSeconds) < 1.0e-9 && std::abs (old.endSeconds - span.endSeconds) < 1.0e-9; });
            const bool touched = std::any_of (sinceLast.begin(), sinceLast.end(), [&] (const TimeSpan& change)
                                              { return change.startSeconds <= span.endSeconds && change.endSeconds >= span.startSeconds; });
            if (sameSpan && ! touched)
            {
                for (size_t c = 0; c < input.size(); ++c)
                    std::copy (previous->output[c].begin() + first, previous->output[c].begin() + last, result.output[c].begin() + first);
                reused = true;
            }
        }
        if (reused)
            continue;
        CorrectionCurve part;
        for (const auto& point : editedCurve.points)
            if (point.timeSeconds >= span.startSeconds && point.timeSeconds <= span.endSeconds)
                part.points.push_back (point);
        const auto rendered = renderCorrection (input, sampleRate, part, sampleZeroSeconds, fundamentalHintHz);
        for (size_t c = 0; c < input.size(); ++c)
            std::copy (rendered[c].begin() + first, rendered[c].begin() + last, result.output[c].begin() + first);
        result.renderedSpans.push_back (span);
    }
    return result;
}

double medianVoicedFrequency (const PitchTrack& track)
{
    std::vector<double> values;
    for (const auto& frame : track.frames)
        if (frame.voiced)
            values.push_back (frame.frequencyHz);
    if (values.empty())
        return 0.0;
    const auto middle = values.begin() + (long) (values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

std::vector<double> correctedPitchMidi (const PitchTrack& track, const CorrectionCurve& curve)
{
    std::vector<double> result (track.frames.size(), 0.0);
    for (size_t i = 0; i < track.frames.size(); ++i)
        if (track.frames[i].voiced)
            result[i] = frequencyToMidi (track.frames[i].frequencyHz) + curve.shiftAt (track.frames[i].timeSeconds);
    return result;
}

} // namespace shizuku
