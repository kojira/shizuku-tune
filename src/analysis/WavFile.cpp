#include "WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace shizuku
{

std::vector<float> WavData::mixToMono() const
{
    if (channels.empty())
        return {};
    std::vector<float> mono (channels[0].size(), 0.0f);
    for (const auto& channel : channels)
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += channel[i];
    const float scale = 1.0f / (float) channels.size();
    for (auto& value : mono)
        value *= scale;
    return mono;
}

namespace
{
uint32_t readU32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
uint16_t readU16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

void writeU32 (std::ofstream& out, uint32_t v)
{
    const unsigned char b[4] = { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24) };
    out.write ((const char*) b, 4);
}
void writeU16 (std::ofstream& out, uint16_t v)
{
    const unsigned char b[2] = { (unsigned char) v, (unsigned char) (v >> 8) };
    out.write ((const char*) b, 2);
}
} // namespace

WavData readWav (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        throw std::runtime_error ("cannot open " + path);
    std::vector<unsigned char> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error ("not a RIFF/WAVE file: " + path);

    uint16_t format = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;

    size_t position = 12;
    while (position + 8 <= bytes.size())
    {
        const unsigned char* chunk = bytes.data() + position;
        const uint32_t chunkSize = readU32 (chunk + 4);
        const size_t available = std::min<size_t> (chunkSize, bytes.size() - position - 8);
        if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
        {
            format = readU16 (chunk + 8);
            numChannels = readU16 (chunk + 10);
            sampleRate = readU32 (chunk + 12);
            bitsPerSample = readU16 (chunk + 22);
            if (format == 0xFFFE && available >= 26)
                format = readU16 (chunk + 8 + 24); // WAVE_FORMAT_EXTENSIBLE: sub-format GUID starts with the format tag
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            data = chunk + 8;
            dataSize = available;
        }
        position += 8 + chunkSize + (chunkSize & 1);
    }

    if (data == nullptr || numChannels == 0 || sampleRate == 0)
        throw std::runtime_error ("missing fmt or data chunk: " + path);
    const bool isFloat = format == 3;
    if (! ((format == 1 && (bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32)) || (isFloat && bitsPerSample == 32)))
        throw std::runtime_error ("unsupported WAV encoding in " + path);

    const size_t bytesPerSample = bitsPerSample / 8;
    const size_t numFrames = dataSize / (bytesPerSample * numChannels);

    WavData result;
    result.sampleRate = sampleRate;
    result.numChannels = numChannels;
    result.channels.assign (numChannels, std::vector<float> (numFrames));
    for (size_t frame = 0; frame < numFrames; ++frame)
    {
        for (size_t channel = 0; channel < numChannels; ++channel)
        {
            const unsigned char* p = data + (frame * numChannels + channel) * bytesPerSample;
            float value = 0.0f;
            if (isFloat)
            {
                uint32_t raw = readU32 (p);
                std::memcpy (&value, &raw, 4);
            }
            else if (bitsPerSample == 16)
                value = (float) (int16_t) readU16 (p) / 32768.0f;
            else if (bitsPerSample == 24)
            {
                int32_t raw = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24) >> 8;
                value = (float) raw / 8388608.0f;
            }
            else
                value = (float) ((double) (int32_t) readU32 (p) / 2147483648.0);
            result.channels[channel][frame] = value;
        }
    }
    return result;
}

void writeWavMono16 (const std::string& path, const std::vector<float>& samples, double sampleRate)
{
    std::ofstream out (path, std::ios::binary);
    if (! out)
        throw std::runtime_error ("cannot write " + path);
    const uint32_t dataBytes = (uint32_t) (samples.size() * 2);
    out.write ("RIFF", 4);
    writeU32 (out, 36 + dataBytes);
    out.write ("WAVEfmt ", 8);
    writeU32 (out, 16);
    writeU16 (out, 1);
    writeU16 (out, 1);
    writeU32 (out, (uint32_t) sampleRate);
    writeU32 (out, (uint32_t) sampleRate * 2);
    writeU16 (out, 2);
    writeU16 (out, 16);
    out.write ("data", 4);
    writeU32 (out, dataBytes);
    for (float sample : samples)
    {
        const auto value = (int16_t) std::lround (std::clamp (sample, -1.0f, 1.0f) * 32767.0f);
        writeU16 (out, (uint16_t) value);
    }
}

void writeWavFloat32 (const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate)
{
    if (channels.empty())
        throw std::runtime_error ("no channels to write to " + path);
    std::ofstream out (path, std::ios::binary);
    if (! out)
        throw std::runtime_error ("cannot write " + path);
    const auto numChannels = (uint16_t) channels.size();
    const size_t numFrames = channels[0].size();
    const uint32_t blockAlign = 4u * numChannels;
    const uint32_t dataBytes = (uint32_t) (numFrames * blockAlign);
    out.write ("RIFF", 4);
    writeU32 (out, 36 + dataBytes);
    out.write ("WAVEfmt ", 8);
    writeU32 (out, 16);
    writeU16 (out, 3); // WAVE_FORMAT_IEEE_FLOAT
    writeU16 (out, numChannels);
    writeU32 (out, (uint32_t) sampleRate);
    writeU32 (out, (uint32_t) sampleRate * blockAlign);
    writeU16 (out, (uint16_t) blockAlign);
    writeU16 (out, 32);
    out.write ("data", 4);
    writeU32 (out, dataBytes);
    for (size_t i = 0; i < numFrames; ++i)
        for (size_t c = 0; c < numChannels; ++c)
        {
            const float sample = i < channels[c].size() ? channels[c][i] : 0.0f;
            uint32_t raw = 0;
            std::memcpy (&raw, &sample, 4);
            writeU32 (out, raw);
        }
}

} // namespace shizuku
