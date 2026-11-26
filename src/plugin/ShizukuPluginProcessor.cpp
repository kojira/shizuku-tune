#include "ShizukuPluginProcessor.h"

#include "ShizukuDocumentController.h"
#include "ShizukuPluginEditor.h"
#include "AraDiagnostics.h"

namespace shizuku
{

using namespace juce;

ShizukuPluginProcessor::ShizukuPluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)
                          .withInput ("Side Chain", AudioChannelSet::stereo(), false))
{
}

void ShizukuPluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
    captureSession.prepare (sampleRate, samplesPerBlock);
}

void ShizukuPluginProcessor::releaseResources()
{
    releaseResourcesForARA();
}

bool ShizukuPluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    if (output != AudioChannelSet::mono() && output != AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != output)
        return false;
    if (layouts.inputBuses.size() < 2)
        return true;
    const auto sideChain = layouts.getChannelSet (true, 1);
    return sideChain.isDisabled() || sideChain == AudioChannelSet::mono() || sideChain == AudioChannelSet::stereo();
}

void ShizukuPluginProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    if (isBoundToARA())
    {
        auto mainBuffer = getBusBuffer (buffer, false, 0);
        processBlockForARA (mainBuffer, isRealtime(), getPlayHead());
        return;
    }
    auto mainIO = getBusBuffer (buffer, true, 0);
    const auto sideChain = getBusCount (true) > 1 ? getBusBuffer (buffer, true, 1) : AudioBuffer<float> (buffer.getArrayOfWritePointers(), 0, buffer.getNumSamples());
    auto* playHead = getPlayHead();
    captureSession.process (mainIO, sideChain, playHead != nullptr ? playHead->getPosition() : Optional<AudioPlayHead::PositionInfo>(), isNonRealtime());
}

void ShizukuPluginProcessor::getStateInformation (MemoryBlock& destination)
{
    if (auto xml = captureSession.saveState().createXml())
        copyXmlToBinary (*xml, destination);
}

void ShizukuPluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        captureSession.restoreState (ValueTree::fromXml (*xml));
}

double ShizukuPluginProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    if (getTailLengthSecondsForARA (tail))
        return tail;
    return 0.0;
}

void ShizukuPluginProcessor::didBindToARA() noexcept
{
    AudioProcessorARAExtension::didBindToARA();
    araNote ("bound");
}

AudioProcessorEditor* ShizukuPluginProcessor::createEditor()
{
    araDiagnostic (String ("editor created, bound to ARA: ") + (isBoundToARA() ? "yes" : "no"));
    return new ShizukuPluginEditor (*this);
}

} // namespace shizuku

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new shizuku::ShizukuPluginProcessor();
}

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    shizuku::araNote ("factoryRequested");
    return juce::ARADocumentControllerSpecialisation::createARAFactory<shizuku::ShizukuDocumentController>();
}
