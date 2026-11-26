#include "SyntheticSignals.h"

#include <cmath>
#include <random>

namespace shizuku::synth
{

namespace
{
constexpr double twoPi = 2.0 * M_PI;

void addTone (std::vector<float>& out, double sampleRate, double start, double end, double frequency, double gain)
{
    const size_t first = (size_t) (start * sampleRate);
    const size_t last = std::min (out.size(), (size_t) (end * sampleRate));
    const double attack = 0.01, release = 0.05;
    for (size_t i = first; i < last; ++i)
    {
        const double t = (double) (i - first) / sampleRate;
        const double remaining = end - start - t;
        double envelope = std::min (1.0, t / attack) * std::min (1.0, remaining / release) * std::exp (-0.6 * t);
        double sample = 0.0;
        for (int harmonic = 1; harmonic <= 8; ++harmonic)
        {
            const double f = frequency * harmonic;
            if (f > sampleRate * 0.45)
                break;
            sample += std::sin (twoPi * f * t) / harmonic;
        }
        out[i] += (float) (gain * envelope * sample);
    }
}
} // namespace

KeySection diatonicSection (const MusicalKey& key)
{
    const int t = key.tonicPitchClass;
    const auto pc = [t] (int interval) { return (t + interval) % 12; };
    KeySection section;
    section.key = key;
    if (key.mode == KeyMode::major)
        section.chords = { { pc (0), false }, { pc (5), false }, { pc (7), false }, { pc (9), true },
                           { pc (2), true },  { pc (7), false }, { pc (0), false }, { pc (0), false } };
    else
        section.chords = { { pc (0), true }, { pc (5), true }, { pc (7), false }, { pc (0), true },
                           { pc (8), false }, { pc (5), true }, { pc (7), false }, { pc (0), true } };
    return section;
}

std::vector<float> renderAccompaniment (const std::vector<KeySection>& sections, double barSeconds, double sampleRate)
{
    size_t numBars = 0;
    for (const auto& section : sections)
        numBars += section.chords.size();
    std::vector<float> out ((size_t) (numBars * barSeconds * sampleRate) + 1, 0.0f);

    size_t bar = 0;
    for (const auto& section : sections)
    {
        for (const auto& chord : section.chords)
        {
            const double start = bar * barSeconds;
            const int rootMidi = 48 + chord.rootPitchClass; // C3..B3
            const int third = chord.minor ? 3 : 4;
            // Two strums per bar, triad around middle C plus a bass note.
            for (int strum = 0; strum < 2; ++strum)
            {
                const double s = start + strum * barSeconds * 0.5;
                const double e = s + barSeconds * 0.5;
                for (int interval : { 0, third, 7 })
                    addTone (out, sampleRate, s, e, midiToFrequency (rootMidi + interval + 12), 0.05);
                addTone (out, sampleRate, s, e, midiToFrequency (rootMidi - 12), 0.07);
            }
            ++bar;
        }
    }
    return out;
}

std::vector<TrueNote> detunedMelody (const MusicalKey& key, int octaveBaseMidi, double startSeconds)
{
    // Scale degrees (semitones above tonic) and detune in cents.
    const bool major = key.mode == KeyMode::major;
    const std::vector<int> degrees = major ? std::vector<int> { 0, 2, 4, 5, 7, 9, 7, 4, 2, 0, 4, 7 }
                                           : std::vector<int> { 0, 2, 3, 5, 7, 8, 7, 3, 2, 0, 3, 7 };
    const std::vector<double> detuneCents { 0.0, 32.0, -27.0, 18.0, -41.0, 12.0, 45.0, -15.0, -35.0, 22.0, 8.0, -48.0 };
    const std::vector<double> lengths { 0.45, 0.30, 0.60, 0.25, 0.80, 0.40, 0.35, 0.55, 0.30, 0.90, 0.40, 0.70 };

    std::vector<TrueNote> notes;
    double t = startSeconds;
    for (size_t i = 0; i < degrees.size(); ++i)
    {
        const double midi = octaveBaseMidi + key.tonicPitchClass + degrees[i] + detuneCents[i] / 100.0;
        notes.push_back ({ t, t + lengths[i], midi });
        t += lengths[i] + (i % 3 == 2 ? 0.25 : 0.08); // short breaths, longer gap every third note
    }
    return notes;
}

std::vector<float> renderVocal (const std::vector<TrueNote>& notes, VoiceTimbre timbre, double sampleRate, double totalSeconds)
{
    std::vector<float> out ((size_t) (totalSeconds * sampleRate) + 1, 0.0f);
    std::mt19937 random (1234);
    std::normal_distribution<double> noise (0.0, 1.0);

    for (const auto& note : notes)
    {
        const size_t first = (size_t) (note.startSeconds * sampleRate);
        const size_t last = std::min (out.size(), (size_t) (note.endSeconds * sampleRate));
        const double duration = note.endSeconds - note.startSeconds;
        double phase = 0.0;
        for (size_t i = first; i < last; ++i)
        {
            const double t = (double) (i - first) / sampleRate;
            double midi = note.midi;
            double sample = 0.0;
            const double envelope = std::min (1.0, t / 0.02) * std::min (1.0, (duration - t) / 0.03);

            if (timbre == VoiceTimbre::sine)
            {
                phase += twoPi * midiToFrequency (midi) / sampleRate;
                sample = 0.3 * std::sin (phase);
            }
            else
            {
                // Scoop up from 60 cents below over 70 ms, vibrato (5.5 Hz, +-25 cents) after 150 ms,
                // fall of 40 cents in the last 60 ms.
                midi += -0.6 * std::exp (-t / 0.025);
                if (t > 0.15)
                    midi += 0.25 * std::sin (twoPi * 5.5 * (t - 0.15)) * std::min (1.0, (t - 0.15) / 0.2);
                if (duration - t < 0.06)
                    midi -= 0.4 * (1.0 - (duration - t) / 0.06);
                const double f0 = midiToFrequency (midi);
                phase += twoPi * f0 / sampleRate;
                for (int harmonic = 1; harmonic <= 30; ++harmonic)
                {
                    const double f = f0 * harmonic;
                    if (f > sampleRate * 0.45)
                        break;
                    // "ah" formants around 800, 1150, 2900 Hz.
                    const auto formant = [f] (double centre, double width, double gain)
                    { return gain / (1.0 + std::pow ((f - centre) / width, 2.0)); };
                    const double amplitude = (formant (800.0, 120.0, 1.0) + formant (1150.0, 150.0, 0.6) + formant (2900.0, 250.0, 0.25) + 0.02)
                                             / std::sqrt ((double) harmonic);
                    sample += amplitude * std::sin (harmonic * phase);
                }
                sample = 0.15 * sample + 0.004 * noise (random);
            }
            out[i] += (float) (envelope * sample);
        }
    }
    return out;
}

} // namespace shizuku::synth
