#pragma once

#include <string>
#include <vector>

namespace shizuku
{

struct WavData
{
    double sampleRate = 0.0;
    int numChannels = 0;
    std::vector<std::vector<float>> channels;

    std::vector<float> mixToMono() const;
};

// Reads RIFF/WAVE PCM 16/24/32-bit and IEEE float 32-bit files. Throws std::runtime_error on failure.
WavData readWav (const std::string& path);

// Writes a mono 16-bit PCM file. Throws std::runtime_error on failure.
void writeWavMono16 (const std::string& path, const std::vector<float>& samples, double sampleRate);

// Writes a 32-bit IEEE float file with any number of equally long channels (no clipping above full scale).
// Throws std::runtime_error on failure.
void writeWavFloat32 (const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate);

} // namespace shizuku
