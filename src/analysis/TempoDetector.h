#pragma once

#include <vector>

namespace shizuku
{

struct TempoEstimate
{
    double bpm = 0.0;        // 0 when no tempo was found
    double confidence = 0.0; // autocorrelation peak height relative to lag 0 (0..1)
};

struct TempoDetectorSettings
{
    double minBpm = 70.0;
    double maxBpm = 200.0;
    double preferredBpm = 120.0;   // centre of the log-normal tempo prior (resolves half/double tempo)
    double priorOctaves = 1.0;     // standard deviation of the prior, in octaves of tempo
};

// Tempo of an accompaniment from the autocorrelation of its spectral-flux onset strength.
TempoEstimate estimateTempo (const std::vector<float>& monoSamples, double sampleRate, const TempoDetectorSettings& settings = {});

} // namespace shizuku
