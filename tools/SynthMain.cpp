// shizuku-synth: writes a known-key accompaniment and a detuned vocal to WAV, plus the truth as JSON.
// Usage: shizuku-synth <outDir> <tonic 0-11> <major|minor> <sine|voice>

#include "../src/analysis/WavFile.h"
#include "SyntheticSignals.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace shizuku;

int main (int argc, char** argv)
{
    if (argc < 5)
    {
        std::cerr << "usage: " << argv[0] << " <outDir> <tonic 0-11> <major|minor> <sine|voice>\n";
        return 2;
    }
    const std::string outDir = argv[1];
    const MusicalKey key { std::atoi (argv[2]) % 12, std::strcmp (argv[3], "minor") == 0 ? KeyMode::minor : KeyMode::major };
    const auto timbre = std::strcmp (argv[4], "sine") == 0 ? synth::VoiceTimbre::sine : synth::VoiceTimbre::voiceLike;
    const double sampleRate = 44100.0;

    const auto accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
    const auto melody = synth::detunedMelody (key, 60, 0.5);
    const auto vocal = synth::renderVocal (melody, timbre, sampleRate, accompaniment.size() / sampleRate);

    try
    {
        writeWavMono16 (outDir + "/vocal.wav", vocal, sampleRate);
        writeWavMono16 (outDir + "/accompaniment.wav", accompaniment, sampleRate);
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }

    std::ofstream truth (outDir + "/truth.json");
    truth << "{\n  \"key\": \"" << keyName (key) << "\",\n  \"notes\": [\n";
    for (size_t i = 0; i < melody.size(); ++i)
    {
        char line[160];
        std::snprintf (line, sizeof (line), "    {\"start\":%.3f,\"end\":%.3f,\"midi\":%.3f}%s\n", melody[i].startSeconds,
                       melody[i].endSeconds, melody[i].midi, i + 1 < melody.size() ? "," : "");
        truth << line;
    }
    truth << "  ]\n}\n";
    std::cout << "wrote " << outDir << "/vocal.wav, accompaniment.wav, truth.json (" << keyName (key) << ")\n";
    return 0;
}
