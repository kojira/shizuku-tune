#include "KeyDetector.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace shizuku
{

namespace
{
// Krumhansl-Kessler probe-tone profiles (tonic = index 0).
constexpr std::array<double, 12> majorProfile { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
constexpr std::array<double, 12> minorProfile { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

double pearson (const std::array<double, 12>& a, const std::array<double, 12>& b)
{
    const double meanA = std::accumulate (a.begin(), a.end(), 0.0) / 12.0;
    const double meanB = std::accumulate (b.begin(), b.end(), 0.0) / 12.0;
    double covariance = 0.0, varianceA = 0.0, varianceB = 0.0;
    for (size_t i = 0; i < 12; ++i)
    {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        covariance += da * db;
        varianceA += da * da;
        varianceB += db * db;
    }
    if (varianceA <= 0.0 || varianceB <= 0.0)
        return 0.0;
    return covariance / std::sqrt (varianceA * varianceB);
}

// Bass and upper chroma summed over [begin, end), each normalised to unit sum, then added.
Chroma sumChroma (const std::vector<ChromaFrame>& frames, size_t begin, size_t end)
{
    Chroma bass {}, upper {};
    for (size_t f = begin; f < end && f < frames.size(); ++f)
        for (size_t i = 0; i < 12; ++i)
        {
            bass[i] += frames[f].bass[i];
            upper[i] += frames[f].upper[i];
        }
    const double bassSum = std::accumulate (bass.begin(), bass.end(), 0.0);
    const double upperSum = std::accumulate (upper.begin(), upper.end(), 0.0);
    Chroma total {};
    for (size_t i = 0; i < 12; ++i)
        total[i] = (bassSum > 0.0 ? bass[i] / bassSum : 0.0) + (upperSum > 0.0 ? upper[i] / upperSum : 0.0);
    return total;
}

Chroma normalisedToMax (Chroma chroma)
{
    const double maximum = *std::max_element (chroma.begin(), chroma.end());
    if (maximum > 0.0)
        for (auto& value : chroma)
            value /= maximum;
    return chroma;
}

int argmax (const std::array<double, 24>& values)
{
    return (int) (std::max_element (values.begin(), values.end()) - values.begin());
}
} // namespace

std::array<double, 24> KeyDetector::scoreKeys (const Chroma& chroma)
{
    std::array<double, 24> scores {};
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        std::array<double, 12> rotatedMajor {}, rotatedMinor {};
        for (int pc = 0; pc < 12; ++pc)
        {
            rotatedMajor[(size_t) pc] = majorProfile[(size_t) ((pc - tonic + 12) % 12)];
            rotatedMinor[(size_t) pc] = minorProfile[(size_t) ((pc - tonic + 12) % 12)];
        }
        scores[(size_t) tonic] = pearson (chroma, rotatedMajor);
        scores[(size_t) (tonic + 12)] = pearson (chroma, rotatedMinor);
    }
    return scores;
}

std::vector<ChromaFrame> KeyDetector::computeChromagram (const std::vector<float>& monoSamples, double sampleRate, double& frameHopSeconds) const
{
    // Long frames give enough frequency resolution to separate semitones in the bass register.
    const int fftSize = Fft::nextPowerOfTwo ((int) std::lround (sampleRate * 0.37));
    const int hop = fftSize / 4;
    frameHopSeconds = hop / sampleRate;

    std::vector<ChromaFrame> frames;
    if ((int) monoSamples.size() < fftSize / 2)
        return frames;

    Fft fft (fftSize);
    std::vector<double> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / fftSize);

    const int numBins = fftSize / 2;
    const int minBin = std::max (2, (int) std::floor (settings.minFrequencyHz * fftSize / sampleRate));
    const int maxBin = std::min (numBins - 2, (int) std::ceil (settings.maxFrequencyHz * fftSize / sampleRate));
    constexpr double thirdOctave = 1.2599210498948732;

    std::vector<std::complex<double>> buffer ((size_t) fftSize);
    std::vector<double> magnitude ((size_t) numBins), prefix ((size_t) numBins + 1);
    const size_t total = monoSamples.size();
    for (size_t start = 0; start + (size_t) fftSize / 2 <= total; start += (size_t) hop)
    {
        for (int i = 0; i < fftSize; ++i)
        {
            const size_t index = start + (size_t) i;
            const double sample = index < total ? monoSamples[index] : 0.0;
            buffer[(size_t) i] = { sample * window[(size_t) i], 0.0 };
        }
        fft.forward (buffer);
        for (int bin = 0; bin < numBins; ++bin)
        {
            magnitude[(size_t) bin] = std::abs (buffer[(size_t) bin]);
            prefix[(size_t) bin + 1] = prefix[(size_t) bin] + magnitude[(size_t) bin];
        }

        // Spectral peaks (magnitude-weighted) that stand clearly above the local (+-1/3 octave) mean, so
        // broadband energy such as drums and the skirts of loud partials do not leak into neighbouring
        // pitch classes.
        ChromaFrame frame;
        for (int bin = minBin; bin <= maxBin; ++bin)
        {
            const double m = magnitude[(size_t) bin];
            if (! (m > magnitude[(size_t) bin - 1] && m >= magnitude[(size_t) bin + 1]))
                continue;
            const int low = std::max (1, (int) (bin / thirdOctave));
            const int high = std::min (numBins - 1, (int) (bin * thirdOctave));
            const double localMean = (prefix[(size_t) high + 1] - prefix[(size_t) low]) / (double) (high - low + 1);
            if (m <= settings.peakOverLocalMean * localMean)
                continue;

            // Parabolic interpolation of the peak frequency on log magnitudes.
            const double a = std::log (magnitude[(size_t) bin - 1] + 1.0e-12), b = std::log (m + 1.0e-12),
                         c = std::log (magnitude[(size_t) bin + 1] + 1.0e-12);
            const double denominator = a - 2.0 * b + c;
            const double offset = denominator < 0.0 ? 0.5 * (a - c) / denominator : 0.0;
            const double frequency = (bin + offset) * sampleRate / fftSize;
            const double midi = frequencyToMidi (frequency);
            const double nearest = std::round (midi);
            if (std::abs (midi - nearest) > 0.3)
                continue;
            const auto pitchClass = (size_t) (((int) nearest % 12 + 12) % 12);
            auto& chroma = frequency < settings.bassSplitHz ? frame.bass : frame.upper;
            chroma[pitchClass] += m;
        }
        frames.push_back (frame);
    }
    return frames;
}

KeyAnalysis KeyDetector::analyse (const std::vector<float>& monoSamples, double sampleRate) const
{
    KeyAnalysis result;
    double frameHop = 0.0;
    const auto frames = computeChromagram (monoSamples, sampleRate, frameHop);
    if (frames.empty())
        return result;

    // Global key.
    const Chroma globalChroma = sumChroma (frames, 0, frames.size());
    result.globalChroma = normalisedToMax (globalChroma);
    const auto globalScores = scoreKeys (globalChroma);
    for (int k = 0; k < 24; ++k)
        result.rankedGlobalKeys.push_back ({ MusicalKey::fromIndex (k), globalScores[(size_t) k] });
    std::sort (result.rankedGlobalKeys.begin(), result.rankedGlobalKeys.end(),
               [] (const KeyScore& a, const KeyScore& b) { return a.correlation > b.correlation; });
    result.globalKey = result.rankedGlobalKeys.front().key;
    result.globalConfidence = result.rankedGlobalKeys.front().correlation;

    // Local windows.
    const double durationSeconds = monoSamples.size() / sampleRate;
    const size_t framesPerWindow = std::max<size_t> (1, (size_t) std::lround (settings.windowSeconds / frameHop));
    const size_t framesPerHop = std::max<size_t> (1, (size_t) std::lround (settings.windowHopSeconds / frameHop));

    struct Window
    {
        double centreSeconds;
        std::array<double, 24> scores;
    };
    std::vector<Window> windows;
    if (frames.size() <= framesPerWindow)
    {
        windows.push_back ({ durationSeconds * 0.5, globalScores });
    }
    else
    {
        for (size_t begin = 0; begin + framesPerWindow <= frames.size() + framesPerHop - 1; begin += framesPerHop)
        {
            const size_t end = std::min (frames.size(), begin + framesPerWindow);
            const Chroma chroma = sumChroma (frames, begin, end);
            const double centre = ((begin + end) * 0.5) * frameHop;
            windows.push_back ({ centre, scoreKeys (chroma) });
            if (end == frames.size())
                break;
        }
    }

    // Viterbi smoothing across windows so short ambiguous passages do not flip the key.
    const size_t numWindows = windows.size();
    std::vector<std::array<double, 24>> pathScore (numWindows);
    std::vector<std::array<int, 24>> backPointer (numWindows);
    pathScore[0] = windows[0].scores;
    for (size_t w = 1; w < numWindows; ++w)
    {
        for (int k = 0; k < 24; ++k)
        {
            double best = -1.0e18;
            int bestPrevious = 0;
            for (int previous = 0; previous < 24; ++previous)
            {
                const double candidate = pathScore[w - 1][(size_t) previous] - (previous == k ? 0.0 : settings.keyChangePenalty);
                if (candidate > best)
                {
                    best = candidate;
                    bestPrevious = previous;
                }
            }
            pathScore[w][(size_t) k] = best + windows[w].scores[(size_t) k];
            backPointer[w][(size_t) k] = bestPrevious;
        }
    }
    std::vector<int> path (numWindows);
    path[numWindows - 1] = argmax (pathScore[numWindows - 1]);
    for (size_t w = numWindows - 1; w > 0; --w)
        path[w - 1] = backPointer[w][(size_t) path[w]];

    // Window-level segments (frame ranges).
    struct FrameSegment
    {
        size_t beginFrame, endFrame;
        int keyIndex;
    };
    const auto secondsToFrame = [&] (double seconds)
    { return std::min (frames.size(), (size_t) std::max (0.0, std::round (seconds / frameHop))); };
    std::vector<FrameSegment> frameSegments;
    for (size_t w = 0; w < numWindows; ++w)
    {
        const double start = w == 0 ? 0.0 : 0.5 * (windows[w - 1].centreSeconds + windows[w].centreSeconds);
        const double end = w + 1 == numWindows ? durationSeconds : 0.5 * (windows[w].centreSeconds + windows[w + 1].centreSeconds);
        if (! frameSegments.empty() && frameSegments.back().keyIndex == path[w])
            frameSegments.back().endFrame = secondsToFrame (end);
        else
            frameSegments.push_back ({ secondsToFrame (start), secondsToFrame (end), path[w] });
    }
    frameSegments.back().endFrame = frames.size();

    // Confirm key changes: merge neighbours whose combined chroma still prefers one of their keys.
    // Local windows over a cadence (e.g. V-vi-ii-V) often look like the dominant key; a real
    // modulation produces a combined chroma that matches neither key.
    for (bool merged = true; merged && frameSegments.size() > 1;)
    {
        merged = false;
        for (size_t i = 0; i + 1 < frameSegments.size(); ++i)
        {
            auto& left = frameSegments[i];
            auto& right = frameSegments[i + 1];
            const int combinedKey = argmax (scoreKeys (sumChroma (frames, left.beginFrame, right.endFrame)));
            if (combinedKey == left.keyIndex || combinedKey == right.keyIndex)
            {
                left.endFrame = right.endFrame;
                left.keyIndex = combinedKey;
                frameSegments.erase (frameSegments.begin() + (long) i + 1);
                merged = true;
                break;
            }
        }
    }

    // Refine each remaining boundary: per-frame correlations are additive, so choose the split that
    // maximises (sum of left-key fit before it) + (sum of right-key fit after it).
    for (size_t i = 0; i + 1 < frameSegments.size(); ++i)
    {
        auto& left = frameSegments[i];
        auto& right = frameSegments[i + 1];
        const size_t begin = left.beginFrame, end = right.endFrame;
        std::vector<double> advantagePrefix (end - begin + 1, 0.0); // sum of (leftFit - rightFit)
        for (size_t f = begin; f < end; ++f)
        {
            const auto scores = scoreKeys (sumChroma (frames, f, f + 1));
            advantagePrefix[f - begin + 1] = advantagePrefix[f - begin] + scores[(size_t) left.keyIndex] - scores[(size_t) right.keyIndex];
        }
        const size_t minimumFrames = std::max<size_t> (1, framesPerHop);
        size_t bestSplit = left.endFrame;
        double bestAdvantage = -1.0e18;
        for (size_t split = begin + minimumFrames; split + minimumFrames <= end; ++split)
        {
            if (advantagePrefix[split - begin] > bestAdvantage)
            {
                bestAdvantage = advantagePrefix[split - begin];
                bestSplit = split;
            }
        }
        left.endFrame = bestSplit;
        right.beginFrame = bestSplit;
    }

    for (const auto& segment : frameSegments)
    {
        const double confidence = scoreKeys (sumChroma (frames, segment.beginFrame, segment.endFrame))[(size_t) segment.keyIndex];
        result.segments.push_back ({ segment.beginFrame * frameHop,
                                     segment.endFrame == frames.size() ? durationSeconds : segment.endFrame * frameHop,
                                     MusicalKey::fromIndex (segment.keyIndex), confidence });
    }
    return result;
}

} // namespace shizuku
