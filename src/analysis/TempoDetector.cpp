#include "TempoDetector.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace shizuku
{

namespace
{
// Linear interpolation of values at a fractional index (0 outside).
double valueAt (const std::vector<double>& values, double index)
{
    if (index < 0.0 || index >= (double) values.size() - 1.0)
        return 0.0;
    const auto i = (size_t) index;
    const double fraction = index - (double) i;
    return values[i] * (1.0 - fraction) + values[i + 1] * fraction;
}
} // namespace

TempoEstimate estimateTempo (const std::vector<float>& monoSamples, double sampleRate, const TempoDetectorSettings& settings)
{
    TempoEstimate estimate;
    constexpr int fftSize = 2048;
    const int hop = std::max (1, (int) std::lround (sampleRate * 0.0058));
    if (sampleRate <= 0.0 || (int) monoSamples.size() < fftSize * 4)
        return estimate;
    const double frameSeconds = hop / sampleRate;

    // Onset strength: half-wave rectified increase of log magnitude, summed over bins.
    Fft fft (fftSize);
    std::vector<double> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / fftSize);
    std::vector<std::complex<double>> buffer ((size_t) fftSize);
    std::vector<double> previous ((size_t) fftSize / 2, 0.0), flux;
    for (size_t start = 0; start + (size_t) fftSize <= monoSamples.size(); start += (size_t) hop)
    {
        for (int i = 0; i < fftSize; ++i)
            buffer[(size_t) i] = { monoSamples[start + (size_t) i] * window[(size_t) i], 0.0 };
        fft.forward (buffer);
        double sum = 0.0;
        for (int bin = 1; bin < fftSize / 2; ++bin)
        {
            const double level = std::log1p (1000.0 * std::abs (buffer[(size_t) bin]));
            sum += std::max (0.0, level - previous[(size_t) bin]);
            previous[(size_t) bin] = level;
        }
        flux.push_back (sum);
    }
    if (flux.size() < 16)
        return estimate;

    // Remove the slowly varying part (local mean over ~1 s), keep the onset peaks.
    const int radius = std::max (1, (int) std::lround (0.5 / frameSeconds));
    std::vector<double> prefix (flux.size() + 1, 0.0);
    for (size_t i = 0; i < flux.size(); ++i)
        prefix[i + 1] = prefix[i] + flux[i];
    std::vector<double> onset (flux.size());
    for (size_t i = 0; i < flux.size(); ++i)
    {
        const size_t lo = (size_t) std::max (0L, (long) i - radius), hi = std::min (flux.size(), i + (size_t) radius + 1);
        onset[i] = std::max (0.0, flux[i] - (prefix[hi] - prefix[lo]) / (double) (hi - lo));
    }

    // Autocorrelation via FFT.
    const int size = Fft::nextPowerOfTwo ((int) onset.size() * 2);
    Fft big (size);
    std::vector<std::complex<double>> spectrum ((size_t) size);
    for (size_t i = 0; i < onset.size(); ++i)
        spectrum[i] = { onset[i], 0.0 };
    big.forward (spectrum);
    for (auto& value : spectrum)
        value = { std::norm (value), 0.0 };
    big.inverse (spectrum);
    const double maxLagSeconds = 16.0 * 60.0 / settings.minBpm * 1.03;
    std::vector<double> autocorrelation ((size_t) std::min ((double) onset.size(), maxLagSeconds / frameSeconds + 2.0));
    for (size_t lag = 0; lag < autocorrelation.size(); ++lag)
        autocorrelation[lag] = spectrum[lag].real() / (double) (onset.size() - lag);
    if (autocorrelation[0] <= 0.0)
        return estimate;

    // Score each tempo (0.05 BPM steps) by the autocorrelation at one to four beats, with a log-normal prior.
    double bestScore = -1.0;
    for (double bpm = settings.minBpm; bpm <= settings.maxBpm; bpm += 0.05)
    {
        const double beatFrames = 60.0 / bpm / frameSeconds;
        double score = 0.0;
        for (int beats = 1; beats <= 4; ++beats)
            score += valueAt (autocorrelation, beats * beatFrames) / (double) beats;
        const double octaves = std::log2 (bpm / settings.preferredBpm) / settings.priorOctaves;
        score *= std::exp (-0.5 * octaves * octaves);
        if (score > bestScore)
        {
            bestScore = score;
            estimate.bpm = bpm;
        }
    }
    // Refine within +-2 % on the peaks of 4..16 beats (long lags resolve the beat length more finely).
    const double coarse = estimate.bpm;
    bestScore = -1.0;
    for (double bpm = coarse * 0.98; bpm <= coarse * 1.02; bpm += 0.01)
    {
        const double beatFrames = 60.0 / bpm / frameSeconds;
        double score = 0.0;
        for (int beats = 4; beats <= 16; ++beats)
            score += valueAt (autocorrelation, beats * beatFrames);
        if (score > bestScore)
        {
            bestScore = score;
            estimate.bpm = bpm;
        }
    }
    estimate.confidence = std::clamp (valueAt (autocorrelation, 60.0 / estimate.bpm / frameSeconds) / autocorrelation[0], 0.0, 1.0);
    return estimate;
}

} // namespace shizuku
