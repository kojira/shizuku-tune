#pragma once

#include "MusicTheory.h"

#include <array>
#include <vector>

namespace shizuku
{

using Chroma = std::array<double, 12>;

// Chroma of one analysis frame, split at bassSplitHz. Bass and upper chroma are normalised separately and
// weighted equally when combined, so the bass line (chord roots) counts as much as the melody and upper
// harmony. Without this, a melody that dwells on the fifth makes the dominant key (e.g. D for G) win.
struct ChromaFrame
{
    Chroma bass {};
    Chroma upper {};
};

struct KeyScore
{
    MusicalKey key;
    double correlation = 0.0; // Pearson correlation with the Krumhansl-Kessler profile
};

struct KeySegment
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    MusicalKey key;
    double confidence = 0.0; // correlation of the chosen key for the segment's chroma
};

struct KeyAnalysis
{
    MusicalKey globalKey;
    double globalConfidence = 0.0;
    std::vector<KeyScore> rankedGlobalKeys; // all 24 keys, best first
    Chroma globalChroma {};                 // normalised so the maximum is 1
    std::vector<KeySegment> segments;       // contiguous regions with a stable key (modulation aware)
};

struct KeyDetectorSettings
{
    double minFrequencyHz = 38.0;
    double maxFrequencyHz = 5000.0;
    double bassSplitHz = 200.0;     // peaks below this go to the bass chroma
    double peakOverLocalMean = 2.0; // a spectral peak counts when it exceeds the +-1/3-octave mean magnitude by this factor
    double windowSeconds = 8.0;     // length of each local key window
    double windowHopSeconds = 2.0;  // hop between local key windows
    double keyChangePenalty = 0.35; // Viterbi penalty (in correlation units) for switching key between windows
};

// Chroma + Krumhansl-Kessler key estimation for the whole signal and per local window.
class KeyDetector
{
public:
    explicit KeyDetector (KeyDetectorSettings settingsIn = {}) : settings (settingsIn) {}

    KeyAnalysis analyse (const std::vector<float>& monoSamples, double sampleRate) const;

    // Correlation of a chroma vector with all 24 key profiles, indexed by MusicalKey::index().
    static std::array<double, 24> scoreKeys (const Chroma& chroma);

    // Per-frame bass/upper chroma from spectral peaks (frame hop in seconds is returned through frameHopSeconds).
    std::vector<ChromaFrame> computeChromagram (const std::vector<float>& monoSamples, double sampleRate, double& frameHopSeconds) const;

private:
    KeyDetectorSettings settings;
};

} // namespace shizuku
