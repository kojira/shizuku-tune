#pragma once

#include "../src/analysis/MusicTheory.h"

#include <string>
#include <vector>

namespace shizuku::synth
{

struct ChordSpec
{
    int rootPitchClass;
    bool minor;
};

struct KeySection
{
    MusicalKey key;
    std::vector<ChordSpec> chords; // one chord per bar
};

// Harmonic-rich chord accompaniment (root-position triads plus bass), one chord per barSeconds.
std::vector<float> renderAccompaniment (const std::vector<KeySection>& sections, double barSeconds, double sampleRate);

// Typical diatonic progressions for a key: I-IV-V-vi-ii-V-I-I (major) / i-iv-V-i-VI-iv-V-i (minor).
KeySection diatonicSection (const MusicalKey& key);

enum class VoiceTimbre
{
    sine,
    voiceLike // harmonics shaped by vowel-like formants, vibrato, onset scoop, breath noise
};

struct TrueNote
{
    double startSeconds;
    double endSeconds;
    double midi; // true centre pitch (fractional)
};

std::vector<float> renderVocal (const std::vector<TrueNote>& notes, VoiceTimbre timbre, double sampleRate, double totalSeconds);

// Melody in the given key with deliberate per-note detuning (cents) so correction targets are known.
std::vector<TrueNote> detunedMelody (const MusicalKey& key, int octaveBaseMidi, double startSeconds);

} // namespace shizuku::synth
