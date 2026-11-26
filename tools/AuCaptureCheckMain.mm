// shizuku-au-capture-check: hosts the built Audio Unit through AudioToolbox like a DAW without ARA would, with the
// accompaniment on the side-chain input, and checks the capture mode end to end: play the song once (capture),
// stop, wait for the analysis / correction, play it again and write what the plug-in outputs.
// Usage: shizuku-au-capture-check <Shizuku Tune.component> <vocal.wav> <accompaniment.wav|-> <out.wav>
//                                 [--start seconds] [--realtime] [--expect corrected.wav]
// The component bundle is loaded into this process and registered under a private subtype, so an installed copy
// (and any running host) is not involved. Captures go to a temporary directory. --start places the song at that
// timeline position; --realtime renders without kAudioUnitProperty_OfflineRender and paces the blocks at real
// time, as a playing host does (the capture then never waits on the audio thread). --expect compares the output with a file (e.g. the output
// of shizuku-correct for the same inputs): prints the best lag and the largest difference; exit 1 if they differ.

#include "../src/analysis/WavFile.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr UInt32 blockSize = 512;

struct HostState
{
    std::vector<float> vocal, accompaniment; // timeline from sample 0 (song placed at startSample)
    bool playing = false;
    Float64 position = 0.0; // timeline sample of the block being rendered
};

float sampleAt (const std::vector<float>& signal, Float64 index)
{
    const auto i = (long long) index;
    return i >= 0 && i < (long long) signal.size() ? signal[(size_t) i] : 0.0f;
}

OSStatus renderInput (void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp* timeStamp, UInt32 bus, UInt32 frames,
                      AudioBufferList* data)
{
    const auto& host = *static_cast<HostState*> (refCon);
    const auto& signal = bus == 0 ? host.vocal : host.accompaniment;
    for (UInt32 b = 0; b < data->mNumberBuffers; ++b)
    {
        auto* out = static_cast<float*> (data->mBuffers[b].mData);
        for (UInt32 i = 0; i < frames; ++i)
            out[i] = sampleAt (signal, timeStamp->mSampleTime + i);
    }
    return noErr;
}

OSStatus transportState (void* refCon, Boolean* isPlaying, Boolean* transportStateChanged, Float64* currentSampleInTimeLine,
                         Boolean* isCycling, Float64* cycleStartBeat, Float64* cycleEndBeat)
{
    const auto& host = *static_cast<HostState*> (refCon);
    if (isPlaying) *isPlaying = host.playing;
    if (transportStateChanged) *transportStateChanged = false;
    if (currentSampleInTimeLine) *currentSampleInTimeLine = host.position;
    if (isCycling) *isCycling = false;
    if (cycleStartBeat) *cycleStartBeat = 0;
    if (cycleEndBeat) *cycleEndBeat = 0;
    return noErr;
}

void runLoopFor (double seconds)
{
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, seconds, false);
}

bool ok (OSStatus status, const char* what)
{
    if (status != noErr)
        std::fprintf (stderr, "error: %s failed (%d)\n", what, (int) status);
    return status == noErr;
}

AudioStreamBasicDescription floatFormat (double rate, UInt32 channels)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = format.mBytesPerFrame = 4;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return format;
}

std::string readFile (const std::string& path)
{
    std::string text;
    if (auto* file = std::fopen (path.c_str(), "rb"))
    {
        char buffer[4096];
        size_t n;
        while ((n = std::fread (buffer, 1, sizeof (buffer), file)) > 0)
            text.append (buffer, n);
        std::fclose (file);
    }
    return text;
}

// Renders timeline [from, to) with the transport playing; returns output channel 0.
std::vector<float> play (AudioUnit unit, HostState& host, long long from, long long to, double& renderSeconds, double paceRate)
{
    std::vector<float> output;
    std::vector<float> left (blockSize), right (blockSize);
    host.playing = true;
    const auto started = CFAbsoluteTimeGetCurrent();
    for (long long position = from; position < to; position += blockSize)
    {
        const auto frames = (UInt32) std::min<long long> (blockSize, to - position);
        AudioBufferList* list = (AudioBufferList*) alloca (sizeof (AudioBufferList) + sizeof (AudioBuffer));
        list->mNumberBuffers = 2;
        list->mBuffers[0] = { 1, frames * 4, left.data() };
        list->mBuffers[1] = { 1, frames * 4, right.data() };
        AudioTimeStamp stamp {};
        stamp.mSampleTime = (Float64) position;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        host.position = (Float64) position;
        AudioUnitRenderActionFlags flags = 0;
        if (! ok (AudioUnitRender (unit, &flags, &stamp, 0, frames, list), "AudioUnitRender"))
            return {};
        output.insert (output.end(), left.begin(), left.begin() + frames);
        if (paceRate > 0.0)
        {
            const double due = (double) (position + frames - from) / paceRate;
            const double ahead = due - (CFAbsoluteTimeGetCurrent() - started);
            if (ahead > 0.0)
                usleep ((useconds_t) (ahead * 1.0e6));
        }
    }
    renderSeconds = CFAbsoluteTimeGetCurrent() - started;
    return output;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf (stderr, "usage: %s <Shizuku Tune.component> <vocal.wav> <accompaniment.wav|-> <out.wav> [--start seconds] [--realtime] [--expect file.wav]\n", argv[0]);
        return 2;
    }
    double startSeconds = 0.0;
    bool realtime = false;
    std::string expectPath;
    for (int i = 5; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--start") == 0 && i + 1 < argc)
            startSeconds = std::atof (argv[++i]);
        else if (std::strcmp (argv[i], "--realtime") == 0)
            realtime = true;
        else if (std::strcmp (argv[i], "--expect") == 0 && i + 1 < argc)
            expectPath = argv[++i];
        else
        {
            std::fprintf (stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    char captureDir[] = "/tmp/shizuku-au-capture-XXXXXX";
    if (mkdtemp (captureDir) == nullptr)
        return 1;
    setenv ("SHIZUKU_TUNE_CAPTURE_DIR", captureDir, 1);
    setenv ("SHIZUKU_TUNE_CAPTURE_ARM_ON_START", "1", 1);

    // Load the bundle and register its factory under a private subtype.
    const std::string bundle = argv[1];
    const std::string executable = bundle + "/Contents/MacOS/Shizuku Tune";
    void* library = dlopen (executable.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::fprintf (stderr, "error: cannot load %s: %s\n", executable.c_str(), dlerror());
        return 1;
    }
    auto factory = (AudioComponentFactoryFunction) dlsym (library, "Shizuku_TuneAUFactory");
    if (factory == nullptr)
    {
        std::fprintf (stderr, "error: no Shizuku_TuneAUFactory in %s\n", executable.c_str());
        return 1;
    }
    AudioComponentDescription description { kAudioUnitType_Effect, 'ShzX', 'Kjra', 0, 0 };
    AudioComponent component = AudioComponentRegister (&description, CFSTR ("Kojira: Shizuku Tune (capture check)"), 0x10000, factory);
    AudioUnit unit = nullptr;
    if (component == nullptr || ! ok (AudioComponentInstanceNew (component, &unit), "AudioComponentInstanceNew"))
        return 1;

    shizuku::WavData vocal, accompaniment;
    try
    {
        vocal = shizuku::readWav (argv[2]);
        if (std::strcmp (argv[3], "-") != 0)
            accompaniment = shizuku::readWav (argv[3]);
    }
    catch (const std::exception& error)
    {
        std::fprintf (stderr, "error: %s\n", error.what());
        return 1;
    }
    const double rate = vocal.sampleRate;
    const auto startSample = (long long) std::llround (startSeconds * rate);
    HostState host;
    host.vocal.assign ((size_t) startSample, 0.0f);
    const auto vocalMono = vocal.mixToMono();
    host.vocal.insert (host.vocal.end(), vocalMono.begin(), vocalMono.end());
    if (! accompaniment.channels.empty())
    {
        host.accompaniment.assign ((size_t) startSample, 0.0f);
        const auto mono = accompaniment.mixToMono();
        host.accompaniment.insert (host.accompaniment.end(), mono.begin(), mono.end());
    }
    const auto end = (long long) host.vocal.size();

    UInt32 inputBuses = 0, size = sizeof (inputBuses);
    AudioUnitGetProperty (unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputBuses, &size);
    std::printf ("input buses: %u (%s)\n", (unsigned) inputBuses, inputBuses >= 2 ? "side chain available" : "no side chain");
    if (inputBuses < 2)
        return 1;

    const auto stereo = floatFormat (rate, 2);
    for (UInt32 bus = 0; bus < 2; ++bus)
        if (! ok (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, bus, &stereo, sizeof (stereo)), "input format"))
            return 1;
    if (! ok (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &stereo, sizeof (stereo)), "output format"))
        return 1;
    UInt32 maxFrames = blockSize;
    AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof (maxFrames));
    for (UInt32 bus = 0; bus < 2; ++bus)
    {
        AURenderCallbackStruct callback { renderInput, &host };
        if (! ok (AudioUnitSetProperty (unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus, &callback, sizeof (callback)), "render callback"))
            return 1;
    }
    HostCallbackInfo callbacks {};
    callbacks.hostUserData = &host;
    callbacks.transportStateProc = transportState;
    if (! ok (AudioUnitSetProperty (unit, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &callbacks, sizeof (callbacks)), "host callbacks"))
        return 1;
    UInt32 offline = realtime ? 0 : 1;
    AudioUnitSetProperty (unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offline, sizeof (offline));
    if (! ok (AudioUnitInitialize (unit), "AudioUnitInitialize"))
        return 1;
    runLoopFor (0.5); // the plug-in arms itself (SHIZUKU_TUNE_CAPTURE_ARM_ON_START)

    double seconds = 0.0;
    const auto firstPass = play (unit, host, startSample, end, seconds, realtime ? rate : 0.0);
    std::printf ("capture pass: %.1f s of audio rendered in %.2f s (%s)\n", (double) (end - startSample) / rate, seconds,
                 realtime ? "real-time flag" : "offline");
    double passThrough = 0.0;
    for (size_t i = 0; i < firstPass.size(); ++i)
        passThrough = std::max (passThrough, (double) std::abs (firstPass[i] - host.vocal[(size_t) startSample + i]));
    std::printf ("capture pass output vs. input: max difference %.3g\n", passThrough);

    // Stop: one block with the transport stopped, then wait for the corrected vocal (capture.json gets "correctedWith").
    host.playing = false;
    {
        std::vector<float> scratch (blockSize * 2);
        AudioBufferList* list = (AudioBufferList*) alloca (sizeof (AudioBufferList) + sizeof (AudioBuffer));
        list->mNumberBuffers = 2;
        list->mBuffers[0] = { 1, blockSize * 4, scratch.data() };
        list->mBuffers[1] = { 1, blockSize * 4, scratch.data() + blockSize };
        AudioTimeStamp stamp {};
        stamp.mSampleTime = (Float64) end;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags = 0;
        AudioUnitRender (unit, &flags, &stamp, 0, blockSize, list);
    }
    std::string captureId;
    const auto waitStart = CFAbsoluteTimeGetCurrent();
    while (CFAbsoluteTimeGetCurrent() - waitStart < 600.0)
    {
        runLoopFor (0.1);
        if (captureId.empty())
            if (auto* dir = opendir (captureDir))
            {
                while (auto* entry = readdir (dir))
                    if (entry->d_name[0] != '.')
                        captureId = entry->d_name;
                closedir (dir);
            }
        if (! captureId.empty() && readFile (std::string (captureDir) + "/" + captureId + "/capture.json").find ("correctedWith") != std::string::npos)
            break;
    }
    if (captureId.empty())
    {
        std::fprintf (stderr, "error: nothing was captured\n");
        return 1;
    }
    std::printf ("capture %s analysed and corrected after %.1f s\n", captureId.c_str(), CFAbsoluteTimeGetCurrent() - waitStart);
    runLoopFor (0.2);

    const auto output = play (unit, host, startSample, end, seconds, 0.0);
    shizuku::writeWavFloat32 (argv[4], { output }, rate);
    double changed = 0.0;
    for (size_t i = 0; i < output.size(); ++i)
        changed = std::max (changed, (double) std::abs (output[i] - host.vocal[(size_t) startSample + i]));
    std::printf ("playback pass -> %s (max difference to the input %.3g)\n", argv[4], changed);

    int status = 0;
    if (! expectPath.empty())
    {
        const auto expected = shizuku::readWav (expectPath).channels[0];
        int bestLag = 0;
        double bestError = 1.0e30;
        for (int lag = -64; lag <= 64; ++lag)
        {
            double error = 0.0;
            for (size_t i = 0; i < output.size(); i += 7)
            {
                const auto j = (long long) i + lag;
                if (j >= 0 && j < (long long) expected.size())
                    error += std::abs (output[i] - expected[(size_t) j]);
            }
            if (error < bestError)
            {
                bestError = error;
                bestLag = lag;
            }
        }
        double maxDifference = 0.0;
        for (size_t i = 0; i < output.size() && i < expected.size(); ++i)
            maxDifference = std::max (maxDifference, (double) std::abs (output[i] - expected[i]));
        std::printf ("vs. %s: best lag %d samples, max difference %.3g\n", expectPath.c_str(), bestLag, maxDifference);
        status = bestLag == 0 && maxDifference < 1.0e-6 ? 0 : 1;
    }

    AudioUnitUninitialize (unit);
    AudioComponentInstanceDispose (unit);
    const std::string remove = std::string ("rm -rf '") + captureDir + "'";
    std::system (remove.c_str());
    return status;
}
