#pragma once

#include <vector>

namespace shizuku
{

struct PitchFrame
{
    double timeSeconds = 0.0;  // centre of the analysis frame
    double frequencyHz = 0.0;  // 0 when unvoiced
    double voicedProbability = 0.0;
    double rmsDb = -120.0;
    bool voiced = false;
};

struct PitchTrack
{
    double hopSeconds = 0.0;
    std::vector<PitchFrame> frames;
};

struct PitchTrackerSettings
{
    double minFrequencyHz = 60.0;
    double maxFrequencyHz = 1200.0;
    double hopSeconds = 0.0058;       // ~256 samples at 44.1 kHz
    int binsPerSemitone = 5;          // 20-cent HMM pitch states
    int numThresholds = 100;
    double betaA = 2.0, betaB = 18.0; // threshold prior Beta(2, 18) as in the pYIN paper
    double boltzmannParameter = 2.0;
    double noTroughProbability = 0.01;
    double maxTransitionOctavesPerSecond = 35.92;
    double voicingSwitchProbability = 0.01;
    double silenceThresholdDb = -60.0; // frames quieter than this are treated as unvoiced
};

// Probabilistic YIN (Mauch & Dixon 2014) with HMM Viterbi decoding, implemented from the paper.
class PitchTracker
{
public:
    explicit PitchTracker (PitchTrackerSettings settingsIn = {}) : settings (settingsIn) {}

    PitchTrack analyse (const std::vector<float>& monoSamples, double sampleRate) const;

private:
    PitchTrackerSettings settings;
};

} // namespace shizuku
