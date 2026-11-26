#include "PitchTracker.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace shizuku
{

namespace
{
// Regularised incomplete beta via continued fraction (Numerical Recipes style), used for the
// threshold prior's CDF.
double betaContinuedFraction (double a, double b, double x)
{
    constexpr int maxIterations = 200;
    constexpr double epsilon = 3.0e-12, tiny = 1.0e-300;
    const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (std::abs (d) < tiny)
        d = tiny;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= maxIterations; ++m)
    {
        const int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::abs (d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs (c) < tiny) c = tiny;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::abs (d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs (c) < tiny) c = tiny;
        d = 1.0 / d;
        const double delta = d * c;
        h *= delta;
        if (std::abs (delta - 1.0) < epsilon)
            break;
    }
    return h;
}

double betaCdf (double x, double a, double b)
{
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double logFront = std::lgamma (a + b) - std::lgamma (a) - std::lgamma (b) + a * std::log (x) + b * std::log (1.0 - x);
    const double front = std::exp (logFront);
    if (x < (a + 1.0) / (a + b + 2.0))
        return front * betaContinuedFraction (a, b, x) / a;
    return 1.0 - front * betaContinuedFraction (b, a, 1.0 - x) / b;
}

struct Candidate
{
    double frequencyHz;
    double probability;
    int bin;
};
} // namespace

PitchTrack PitchTracker::analyse (const std::vector<float>& monoSamples, double sampleRate) const
{
    PitchTrack track;
    const int hop = std::max (1, (int) std::lround (settings.hopSeconds * sampleRate));
    track.hopSeconds = hop / sampleRate;

    const int tauMin = std::max (2, (int) std::floor (sampleRate / settings.maxFrequencyHz));
    const int tauMax = (int) std::ceil (sampleRate / settings.minFrequencyHz);
    const int window = tauMax;                // integration window: one period of the lowest pitch
    const int frameLength = window + tauMax + 2;
    const int fftSize = Fft::nextPowerOfTwo (frameLength);
    Fft fft (fftSize);

    const int numBinsPerOctave = 12 * settings.binsPerSemitone;
    const int numPitchBins = (int) std::floor (numBinsPerOctave * std::log2 (settings.maxFrequencyHz / settings.minFrequencyHz)) + 1;
    const auto frequencyToBin = [&] (double frequency)
    { return (int) std::lround (numBinsPerOctave * std::log2 (frequency / settings.minFrequencyHz)); };
    const auto binToFrequency = [&] (double bin)
    { return settings.minFrequencyHz * std::pow (2.0, bin / numBinsPerOctave); };

    // Threshold prior probabilities.
    const int numThresholds = settings.numThresholds;
    std::vector<double> thresholds ((size_t) numThresholds + 1);
    for (int i = 0; i <= numThresholds; ++i)
        thresholds[(size_t) i] = (double) i / numThresholds;
    std::vector<double> thresholdProbability ((size_t) numThresholds);
    for (int i = 0; i < numThresholds; ++i)
        thresholdProbability[(size_t) i] = betaCdf (thresholds[(size_t) i + 1], settings.betaA, settings.betaB)
                                           - betaCdf (thresholds[(size_t) i], settings.betaA, settings.betaB);

    // Signal padded so frame i is centred on sample i * hop.
    const int pad = frameLength / 2;
    const size_t numSamples = monoSamples.size();
    const auto sampleAt = [&] (long index) -> double
    {
        index -= pad;
        return (index >= 0 && (size_t) index < numSamples) ? monoSamples[(size_t) index] : 0.0;
    };
    const size_t numFrames = numSamples == 0 ? 0 : 1 + numSamples / (size_t) hop;

    std::vector<std::complex<double>> spectrumA ((size_t) fftSize), spectrumB ((size_t) fftSize);
    std::vector<double> frame ((size_t) frameLength), squaredPrefix ((size_t) frameLength + 1);
    std::vector<double> difference ((size_t) tauMax + 1), cmndf ((size_t) tauMax + 1);

    std::vector<std::vector<Candidate>> candidates (numFrames);
    std::vector<double> voicedProbability (numFrames, 0.0);
    track.frames.resize (numFrames);

    for (size_t f = 0; f < numFrames; ++f)
    {
        const long start = (long) (f * (size_t) hop);
        for (int i = 0; i < frameLength; ++i)
            frame[(size_t) i] = sampleAt (start + i);

        auto& outFrame = track.frames[f];
        outFrame.timeSeconds = (double) (f * (size_t) hop) / sampleRate;

        // RMS over the centre of the frame (one integration window).
        double energy = 0.0;
        const int centreStart = (frameLength - window) / 2;
        for (int i = 0; i < window; ++i)
            energy += frame[(size_t) (centreStart + i)] * frame[(size_t) (centreStart + i)];
        const double rms = std::sqrt (energy / window);
        outFrame.rmsDb = rms > 0.0 ? 20.0 * std::log10 (rms) : -120.0;

        // Difference function d(tau) = e(0) + e(tau) - 2 r(tau), with r computed via FFT.
        for (int i = 0; i < fftSize; ++i)
        {
            spectrumA[(size_t) i] = { i < window ? frame[(size_t) i] : 0.0, 0.0 };
            spectrumB[(size_t) i] = { i < frameLength ? frame[(size_t) i] : 0.0, 0.0 };
        }
        fft.forward (spectrumA);
        fft.forward (spectrumB);
        for (int i = 0; i < fftSize; ++i)
            spectrumA[(size_t) i] = std::conj (spectrumA[(size_t) i]) * spectrumB[(size_t) i];
        fft.inverse (spectrumA);

        squaredPrefix[0] = 0.0;
        for (int i = 0; i < frameLength; ++i)
            squaredPrefix[(size_t) i + 1] = squaredPrefix[(size_t) i] + frame[(size_t) i] * frame[(size_t) i];
        const double energyAtZero = squaredPrefix[(size_t) window];
        for (int tau = 0; tau <= tauMax; ++tau)
        {
            const double energyAtTau = squaredPrefix[(size_t) (tau + window)] - squaredPrefix[(size_t) tau];
            difference[(size_t) tau] = std::max (0.0, energyAtZero + energyAtTau - 2.0 * spectrumA[(size_t) tau].real());
        }

        // Cumulative mean normalised difference.
        cmndf[0] = 1.0;
        double runningSum = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            runningSum += difference[(size_t) tau];
            cmndf[(size_t) tau] = runningSum > 0.0 ? difference[(size_t) tau] * tau / runningSum : 1.0;
        }

        if (outFrame.rmsDb < settings.silenceThresholdDb)
            continue;

        // Local minima (troughs) in the allowed lag range.
        std::vector<int> troughs;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (cmndf[(size_t) tau] < cmndf[(size_t) tau - 1] && cmndf[(size_t) tau] <= cmndf[(size_t) tau + 1])
                troughs.push_back (tau);
        if (troughs.empty())
            continue;

        // pYIN trough probabilities with a Boltzmann prior over trough order (smaller lags favoured).
        std::vector<double> troughProbability (troughs.size(), 0.0);
        const double lambda = settings.boltzmannParameter;
        for (int t = 0; t < numThresholds; ++t)
        {
            const double threshold = thresholds[(size_t) t + 1];
            int troughsBelow = 0;
            for (int tau : troughs)
                if (cmndf[(size_t) tau] < threshold)
                    ++troughsBelow;
            if (troughsBelow == 0)
                continue;
            const double normaliser = (1.0 - std::exp (-lambda)) / (1.0 - std::exp (-lambda * troughsBelow));
            int position = 0;
            for (size_t k = 0; k < troughs.size(); ++k)
            {
                if (cmndf[(size_t) troughs[k]] >= threshold)
                    continue;
                troughProbability[k] += thresholdProbability[(size_t) t] * normaliser * std::exp (-lambda * position);
                ++position;
            }
        }

        size_t globalMinimum = 0;
        for (size_t k = 1; k < troughs.size(); ++k)
            if (cmndf[(size_t) troughs[k]] < cmndf[(size_t) troughs[globalMinimum]])
                globalMinimum = k;
        double massWithoutTrough = 0.0;
        for (int t = 0; t < numThresholds; ++t)
            if (cmndf[(size_t) troughs[globalMinimum]] >= thresholds[(size_t) t + 1])
                massWithoutTrough += thresholdProbability[(size_t) t];
        troughProbability[globalMinimum] += settings.noTroughProbability * massWithoutTrough;

        double totalProbability = 0.0;
        for (size_t k = 0; k < troughs.size(); ++k)
        {
            if (troughProbability[k] <= 0.0)
                continue;
            const int tau = troughs[k];
            // Parabolic interpolation of the trough on the CMNDF.
            const double left = cmndf[(size_t) tau - 1], centre = cmndf[(size_t) tau], right = cmndf[(size_t) tau + 1];
            const double denominator = left - 2.0 * centre + right;
            double shift = denominator > 0.0 ? 0.5 * (left - right) / denominator : 0.0;
            shift = std::clamp (shift, -0.5, 0.5);
            const double frequency = sampleRate / (tau + shift);
            const int bin = frequencyToBin (frequency);
            if (bin < 0 || bin >= numPitchBins)
                continue;
            candidates[f].push_back ({ frequency, troughProbability[k], bin });
            totalProbability += troughProbability[k];
        }
        voicedProbability[f] = std::clamp (totalProbability, 0.0, 1.0);
    }

    // HMM: states [0, numPitchBins) voiced, [numPitchBins, 2*numPitchBins) unvoiced at that pitch.
    const int maxSemitonesPerFrame = std::max (1, (int) std::lround (settings.maxTransitionOctavesPerSecond * 12.0 * track.hopSeconds));
    const int transitionHalfWidth = maxSemitonesPerFrame * settings.binsPerSemitone;
    std::vector<double> logTransition ((size_t) transitionHalfWidth + 1);
    {
        // Triangular transition kernel over bin offsets -halfWidth..halfWidth.
        double total = 0.0;
        for (int offset = -transitionHalfWidth; offset <= transitionHalfWidth; ++offset)
            total += transitionHalfWidth + 1 - std::abs (offset);
        for (int offset = 0; offset <= transitionHalfWidth; ++offset)
            logTransition[(size_t) offset] = std::log ((transitionHalfWidth + 1 - offset) / total);
    }
    const double logStay = std::log (1.0 - settings.voicingSwitchProbability);
    const double logSwitch = std::log (settings.voicingSwitchProbability);
    const int numStates = 2 * numPitchBins;
    constexpr double logZero = -1.0e30;

    if (numFrames == 0)
        return track;

    std::vector<double> previous ((size_t) numStates), current ((size_t) numStates), observation ((size_t) numStates);
    std::vector<std::vector<int16_t>> backPointers (numFrames, std::vector<int16_t> ((size_t) numStates, 0)); // numStates < 32768

    const auto computeObservation = [&] (size_t f)
    {
        std::fill (observation.begin(), observation.end(), 0.0);
        for (const auto& candidate : candidates[f])
            observation[(size_t) candidate.bin] += candidate.probability;
        const double unvoicedProbability = (1.0 - voicedProbability[f]) / numPitchBins;
        for (int s = 0; s < numPitchBins; ++s)
        {
            observation[(size_t) s] = std::log (std::max (observation[(size_t) s], 1.0e-12));
            observation[(size_t) (s + numPitchBins)] = std::log (std::max (unvoicedProbability, 1.0e-12));
        }
    };

    computeObservation (0);
    for (int s = 0; s < numStates; ++s)
        previous[(size_t) s] = observation[(size_t) s] - std::log ((double) numStates);

    for (size_t f = 1; f < numFrames; ++f)
    {
        computeObservation (f);
        for (int s = 0; s < numStates; ++s)
        {
            const bool targetVoiced = s < numPitchBins;
            const int targetBin = targetVoiced ? s : s - numPitchBins;
            double best = logZero;
            int bestState = s;
            for (int offset = -transitionHalfWidth; offset <= transitionHalfWidth; ++offset)
            {
                const int sourceBin = targetBin + offset;
                if (sourceBin < 0 || sourceBin >= numPitchBins)
                    continue;
                const double transition = logTransition[(size_t) std::abs (offset)];
                const double fromVoiced = previous[(size_t) sourceBin] + transition + (targetVoiced ? logStay : logSwitch);
                const double fromUnvoiced = previous[(size_t) (sourceBin + numPitchBins)] + transition + (targetVoiced ? logSwitch : logStay);
                if (fromVoiced > best)
                {
                    best = fromVoiced;
                    bestState = sourceBin;
                }
                if (fromUnvoiced > best)
                {
                    best = fromUnvoiced;
                    bestState = sourceBin + numPitchBins;
                }
            }
            current[(size_t) s] = best + observation[(size_t) s];
            backPointers[f][(size_t) s] = (int16_t) bestState;
        }
        // Renormalise to keep numbers in range.
        const double maximum = *std::max_element (current.begin(), current.end());
        for (auto& value : current)
            value -= maximum;
        std::swap (previous, current);
    }

    std::vector<int> path (numFrames);
    path[numFrames - 1] = (int) (std::max_element (previous.begin(), previous.end()) - previous.begin());
    for (size_t f = numFrames - 1; f > 0; --f)
        path[f - 1] = backPointers[f][(size_t) path[f]];

    for (size_t f = 0; f < numFrames; ++f)
    {
        auto& outFrame = track.frames[f];
        outFrame.voicedProbability = voicedProbability[f];
        const int state = path[f];
        if (state >= numPitchBins)
            continue;

        // Refine the quantised HMM bin with the most probable nearby trough's exact frequency.
        double frequency = binToFrequency (state);
        double bestProbability = 0.0;
        for (const auto& candidate : candidates[f])
        {
            if (std::abs (candidate.bin - state) <= settings.binsPerSemitone / 2 + 1 && candidate.probability > bestProbability)
            {
                bestProbability = candidate.probability;
                frequency = candidate.frequencyHz;
            }
        }
        outFrame.voiced = true;
        outFrame.frequencyHz = frequency;
    }
    return track;
}

} // namespace shizuku
