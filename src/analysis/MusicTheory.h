#pragma once

#include <array>
#include <cmath>
#include <string>

namespace shizuku
{

enum class KeyMode
{
    major,
    minor
};

struct MusicalKey
{
    int tonicPitchClass = 0; // 0 = C ... 11 = B
    KeyMode mode = KeyMode::major;

    bool operator== (const MusicalKey& other) const noexcept
    {
        return tonicPitchClass == other.tonicPitchClass && mode == other.mode;
    }
    bool operator!= (const MusicalKey& other) const noexcept { return ! (*this == other); }

    // Index 0..23: major keys 0..11, minor keys 12..23.
    int index() const noexcept { return tonicPitchClass + (mode == KeyMode::minor ? 12 : 0); }
    static MusicalKey fromIndex (int keyIndex) noexcept
    {
        return { keyIndex % 12, keyIndex >= 12 ? KeyMode::minor : KeyMode::major };
    }
};

inline const char* pitchClassName (int pitchClass)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((pitchClass % 12) + 12) % 12];
}

inline std::string keyName (const MusicalKey& key)
{
    return std::string (pitchClassName (key.tonicPitchClass)) + (key.mode == KeyMode::major ? " major" : " minor");
}

inline std::string midiNoteName (int midiNote)
{
    return std::string (pitchClassName (midiNote)) + std::to_string (midiNote / 12 - 1);
}

inline double frequencyToMidi (double frequencyHz) { return 69.0 + 12.0 * std::log2 (frequencyHz / 440.0); }
inline double midiToFrequency (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }

// Pitch classes (relative to the tonic) of the scale used for snapping notes.
// Minor uses natural minor plus the raised 7th (harmonic minor leading tone), because
// both appear in typical minor-key melodies.
inline bool isScaleDegree (const MusicalKey& key, int pitchClass)
{
    static constexpr std::array<bool, 12> majorScale { true, false, true, false, true, true, false, true, false, true, false, true };
    static constexpr std::array<bool, 12> minorScale { true, false, true, true, false, true, false, true, true, false, true, true };
    const int relative = ((pitchClass - key.tonicPitchClass) % 12 + 12) % 12;
    return key.mode == KeyMode::major ? majorScale[(size_t) relative] : minorScale[(size_t) relative];
}

// Nearest MIDI note (integer) that belongs to the key's scale.
inline int nearestScaleMidiNote (const MusicalKey& key, double midi)
{
    const int base = (int) std::lround (midi);
    int best = base;
    double bestDistance = 1.0e9;
    for (int candidate = base - 2; candidate <= base + 2; ++candidate)
    {
        if (! isScaleDegree (key, ((candidate % 12) + 12) % 12))
            continue;
        const double distance = std::abs (midi - candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

} // namespace shizuku
