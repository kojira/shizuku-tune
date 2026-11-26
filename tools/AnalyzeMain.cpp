// shizuku-analyze: key of the accompaniment and note list of the vocal, as JSON.
// Usage: shizuku-analyze <vocal.wav|-> <accompaniment.wav|-> [--pitch]

#include "../src/analysis/SongAnalyzer.h"
#include "../src/analysis/WavFile.h"

#include <cstring>
#include <iostream>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <vocal.wav|-> <accompaniment.wav|-> [--pitch]\n";
        return 2;
    }
    const bool includePitch = argc > 3 && std::strcmp (argv[3], "--pitch") == 0;
    try
    {
        std::vector<float> vocal, accompaniment;
        double vocalRate = 0.0, accompanimentRate = 0.0;
        if (std::strcmp (argv[1], "-") != 0)
        {
            const auto wav = shizuku::readWav (argv[1]);
            vocal = wav.mixToMono();
            vocalRate = wav.sampleRate;
        }
        if (std::strcmp (argv[2], "-") != 0)
        {
            const auto wav = shizuku::readWav (argv[2]);
            accompaniment = wav.mixToMono();
            accompanimentRate = wav.sampleRate;
        }
        const auto analysis = shizuku::analyseSong (vocal, vocalRate, accompaniment, accompanimentRate);
        std::cout << shizuku::toJson (analysis, includePitch);
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
