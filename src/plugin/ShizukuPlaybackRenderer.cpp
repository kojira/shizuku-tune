#include "ShizukuPlaybackRenderer.h"

#include "ShizukuDocumentController.h"

namespace shizuku
{

using namespace juce;

namespace
{
// Sample of `audio` channel `channel` at source position `index` (outside the stored range: 0).
inline float sampleAt (const SourceAudio& audio, size_t channel, int64 index) noexcept
{
    const auto local = index - audio.firstSample;
    const auto& data = audio.channels[channel];
    return local >= 0 && local < (int64) data.size() ? data[(size_t) local] : 0.0f;
}

// Reads from the corrected excerpt where it covers the position, otherwise from the original source.
struct RegionAudio
{
    const SourceAudio* original = nullptr;
    const SourceAudio* corrected = nullptr;

    float at (size_t channel, int64 index) const noexcept
    {
        if (corrected != nullptr && index >= corrected->firstSample && index < corrected->firstSample + corrected->numSamples())
            return sampleAt (*corrected, channel, index);
        return sampleAt (*original, channel, index);
    }

    // Catmull-Rom interpolation at a fractional source position.
    float interpolated (size_t channel, double position) const noexcept
    {
        const auto i = (int64) std::floor (position);
        const auto t = (float) (position - (double) i);
        const float p0 = at (channel, i - 1), p1 = at (channel, i), p2 = at (channel, i + 1), p3 = at (channel, i + 2);
        return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    }
};
} // namespace

ShizukuPlaybackRenderer::ShizukuPlaybackRenderer (ARA::PlugIn::DocumentController* documentController,
                                                  ShizukuDocumentController& ownerIn)
    : ARAPlaybackRenderer (documentController), owner (ownerIn)
{
}

void ShizukuPlaybackRenderer::prepareToPlay (double sampleRateIn, int, int numChannelsIn,
                                             AudioProcessor::ProcessingPrecision, AlwaysNonRealtime)
{
    numChannels = numChannelsIn;
    sampleRate = sampleRateIn;

    // Read every source this renderer plays into memory before playback starts (blocking, not real-time).
    std::vector<ARAAudioSource*> sources;
    for (auto* region : getPlaybackRegions())
        sources.push_back (region->getAudioModification()->getAudioSource());
    owner.loadSourcesNow (sources);
}

void ShizukuPlaybackRenderer::releaseResources()
{
}

bool ShizukuPlaybackRenderer::processBlock (AudioBuffer<float>& buffer, AudioProcessor::Realtime,
                                            const AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    buffer.clear();
    const auto lock = owner.getProcessingLock();
    if (! lock.isLocked() || ! positionInfo.getIsPlaying())
        return true;

    const auto data = owner.getPlaybackData();
    const auto numSamples = buffer.getNumSamples();
    const auto outputChannels = (size_t) jmin (numChannels, buffer.getNumChannels());
    const auto blockStart = positionInfo.getTimeInSamples().orFallback (0);
    const auto blockRange = Range<int64>::withStartAndLength (blockStart, numSamples);
    bool success = true;

    for (auto* region : getPlaybackRegions())
    {
        const auto playbackRange = region->getSampleRange (sampleRate, ARAPlaybackRegion::IncludeHeadAndTail::no);
        const auto renderRange = blockRange.getIntersectionWith (playbackRange);
        if (renderRange.isEmpty())
            continue;

        auto* source = region->getAudioModification()->getAudioSource();
        const auto sourceIt = data->sources.find (source);
        if (sourceIt == data->sources.end())
        {
            owner.requestMissingSources();
            success = false;
            continue;
        }
        RegionAudio audio;
        audio.original = sourceIt->second.get();
        if (const auto correctedIt = data->corrected.find (region); correctedIt != data->corrected.end())
            audio.corrected = correctedIt->second.get();

        const auto sourceChannels = audio.original->channels.size();
        if (sourceChannels == 0)
            continue;
        const double sourceRate = audio.original->sampleRate;
        const bool sameRate = exactlyEqual (sourceRate, sampleRate);
        const auto modificationStart = region->getStartInAudioModificationSamples();
        const auto modificationEnd = region->getEndInAudioModificationSamples();
        const double regionStartSeconds = region->getStartInPlaybackTime();

        for (auto position = renderRange.getStart(); position < renderRange.getEnd(); ++position)
        {
            const auto outIndex = (int) (position - blockStart);
            // Source position of this output sample.
            const double sourcePosition = sameRate ? (double) (modificationStart + (position - playbackRange.getStart()))
                                                   : (double) modificationStart + ((double) position / sampleRate - regionStartSeconds) * sourceRate;
            if (sourcePosition < (double) modificationStart || sourcePosition >= (double) modificationEnd)
                continue;
            const auto exactIndex = (int64) sourcePosition;

            const auto read = [&] (size_t channel)
            { return sameRate ? audio.at (channel, exactIndex) : audio.interpolated (channel, sourcePosition); };

            if (sourceChannels == outputChannels)
            {
                for (size_t c = 0; c < outputChannels; ++c)
                    buffer.addSample ((int) c, outIndex, read (c));
            }
            else if (outputChannels == 1)
            {
                float sum = 0.0f;
                for (size_t c = 0; c < sourceChannels; ++c)
                    sum += read (c);
                buffer.addSample (0, outIndex, sum / (float) sourceChannels);
            }
            else if (sourceChannels == 1)
            {
                const float value = read (0);
                for (size_t c = 0; c < outputChannels; ++c)
                    buffer.addSample ((int) c, outIndex, value);
            }
            else
            {
                for (size_t c = 0; c < outputChannels; ++c)
                    buffer.addSample ((int) c, outIndex, read (c % sourceChannels));
            }
        }
    }
    return success;
}

} // namespace shizuku
