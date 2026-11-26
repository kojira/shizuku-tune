#include "ShizukuPluginEditor.h"

#include "ShizukuPluginProcessor.h"
#include "AraDiagnostics.h"

namespace shizuku
{

using namespace juce;

namespace
{
constexpr int noTrackId = 1;
} // namespace

//==============================================================================
ShizukuPluginEditor::ShizukuPluginEditor (ShizukuPluginProcessor& pluginProcessor)
    : AudioProcessorEditor (&pluginProcessor), AudioProcessorEditorARAExtension (&pluginProcessor), shizukuProcessor (pluginProcessor)
{
    for (auto* component : std::initializer_list<Component*> { &vocalLabel, &accompanimentLabel, &vocalChoice, &accompanimentChoice,
                                                                 &analyseButton, &keyLabel, &statusLabel, &pitchLineView,
                                                                 &strengthLabel, &strengthSlider, &thresholdLabel, &thresholdSlider,
                                                                 &referenceButton, &clearReferenceButton, &referenceLabel,
                                                                 &referenceChoiceLabel, &referenceChoice, &captureButton, &finishButton,
                                                                 &captureRangeLabel })
        addAndMakeVisible (component);

    strengthSlider.setRange (0.0, 100.0, 1.0);
    strengthSlider.setTextValueSuffix (" %");
    thresholdSlider.setRange (0.0, 50.0, 1.0);
    thresholdSlider.setTextValueSuffix (" cents");
    for (auto* slider : { &strengthSlider, &thresholdSlider })
    {
        slider->setSliderStyle (Slider::LinearHorizontal);
        slider->setTextBoxStyle (Slider::TextBoxRight, false, 80, 20);
        slider->setChangeNotificationOnlyOnRelease (true); // one re-render per drag
        slider->onValueChange = [this] { applySliderSettings(); };
    }
    referenceLabel.setColour (Label::textColourId, Colours::lightgrey);
    referenceButton.onClick = [this]
    {
        if (model == nullptr)
            return;
        referenceChooser = std::make_unique<FileChooser> ("Reference melody candidates (Synthesizer V .svp, MIDI .mid, or guide .wav)",
                                                          File(), "*.svp;*.mid;*.midi;*.wav");
        referenceChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                           | FileBrowserComponent::canSelectMultipleItems,
                                       [this] (const FileChooser& chooser)
                                       {
                                           if (model == nullptr)
                                               return;
                                           for (const auto& file : chooser.getResults())
                                           {
                                               const auto error = model->addReferenceMelody (file);
                                               if (error.isNotEmpty())
                                                   statusLabel.setText (error, dontSendNotification);
                                           }
                                       });
    };
    clearReferenceButton.onClick = [this]
    {
        if (model != nullptr)
            model->clearReferenceMelodies();
    };
    referenceChoice.onChange = [this]
    {
        if (model != nullptr && referenceChoice.getSelectedId() > 0)
            model->setReferenceChoice (referenceChoice.getSelectedId() - 3);
    };

    keyLabel.setFont (FontOptions (20.0f, Font::bold));
    keyLabel.setJustificationType (Justification::centredRight);
    statusLabel.setColour (Label::textColourId, Colours::lightgrey);

    vocalChoice.onChange = [this]
    {
        if (documentController == nullptr)
            return;
        const int index = vocalChoice.getSelectedId() - 2;
        documentController->setVocalTrack (index >= 0 && index < (int) trackChoices.size() ? trackChoices[(size_t) index] : nullptr);
    };
    accompanimentChoice.onChange = [this]
    {
        if (documentController == nullptr)
            return;
        const int index = accompanimentChoice.getSelectedId() - 2;
        documentController->setAccompanimentTrack (index >= 0 && index < (int) trackChoices.size() ? trackChoices[(size_t) index] : nullptr);
    };
    analyseButton.onClick = [this]
    {
        if (documentController == nullptr)
            return;
        const auto error = documentController->startAnalysis();
        if (error.isNotEmpty())
            statusLabel.setText (error, dontSendNotification);
    };

    captureRangeLabel.setColour (Label::textColourId, Colours::lightgrey);
    captureButton.onClick = [this]
    {
        auto& session = shizukuProcessor.getCaptureSession();
        if (session.getState() == CaptureSession::State::armed)
            session.cancelArm();
        else
            session.arm();
    };
    finishButton.setButtonText (String::fromUTF8 (u8"終了"));
    finishButton.onClick = [this] { shizukuProcessor.getCaptureSession().finishCapture(); };

    if (! attachToARA())
    {
        showCaptureMode (true);
        startTimer (250);
    }

    // ARA editors must be resizable for host integration.
    setResizable (true, false);
    setResizeLimits (720, 360, 4000, 2000);
    setSize (980, 520);
}

bool ShizukuPluginEditor::attachToARA()
{
    if (documentController != nullptr)
        return true;
    auto* editorView = getARAEditorView();
    if (editorView == nullptr)
        return false;
    documentController = ARADocumentControllerSpecialisation::getSpecialisedDocumentController<ShizukuDocumentController> (
        editorView->getDocumentController());
    if (documentController == nullptr)
        return false;
    showCaptureMode (false);
    setModel (documentController);
    araDiagnostic ("editor attached to the ARA document controller");
    return true;
}

void ShizukuPluginEditor::setModel (CorrectionModel* newModel)
{
    if (model != nullptr)
        model->removeChangeListener (this);
    model = newModel;
    if (model == nullptr)
        return;
    const auto settings = model->getCorrectionSettings();
    strengthSlider.setValue (settings.strength * 100.0, dontSendNotification);
    thresholdSlider.setValue (settings.thresholdCents, dontSendNotification);
    model->addChangeListener (this);
    changeListenerCallback (model);
}

void ShizukuPluginEditor::showCaptureMode (bool shouldShow)
{
    for (auto* component : std::initializer_list<Component*> { &vocalLabel, &accompanimentLabel, &vocalChoice, &accompanimentChoice, &analyseButton })
        component->setVisible (! shouldShow);
    for (auto* component : std::initializer_list<Component*> { &captureButton, &finishButton, &captureRangeLabel })
        component->setVisible (shouldShow);
    strengthLabel.setText (shouldShow ? String::fromUTF8 (u8"強さ") : String ("Strength"), dontSendNotification);
    thresholdLabel.setText (shouldShow ? String::fromUTF8 (u8"しきい値") : String ("Threshold"), dontSendNotification);
    thresholdSlider.setTextValueSuffix (shouldShow ? String::fromUTF8 (u8" セント") : String (" cents"));
    referenceButton.setButtonText (shouldShow ? String::fromUTF8 (u8"正解を追加…") : String ("Add reference..."));
    clearReferenceButton.setButtonText (shouldShow ? String::fromUTF8 (u8"消す") : String ("Clear"));
    referenceChoiceLabel.setText (shouldShow ? String::fromUTF8 (u8"正解のメロディー") : String ("Correct melody"), dontSendNotification);
    if (shouldShow)
        setModel (&shizukuProcessor.getCaptureSession());
    resized();
}

void ShizukuPluginEditor::refreshCapture()
{
    if (documentController != nullptr)
        return;
    const auto& session = shizukuProcessor.getCaptureSession();
    const auto state = session.getState();
    const bool hasCapture = session.getCaptureId().isNotEmpty();
    captureButton.setButtonText (state == CaptureSession::State::armed ? String::fromUTF8 (u8"待機をやめる")
                                 : hasCapture || state == CaptureSession::State::capturing ? String::fromUTF8 (u8"取り込み直し")
                                                                                            : String::fromUTF8 (u8"取り込み"));
    captureButton.setEnabled (state != CaptureSession::State::capturing);
    finishButton.setEnabled (state == CaptureSession::State::capturing);
    captureRangeLabel.setText (session.getRangeText(), dontSendNotification);
}

void ShizukuPluginEditor::timerCallback()
{
    if (attachToARA())
        stopTimer();
}

ShizukuPluginEditor::~ShizukuPluginEditor()
{
    stopTimer();
    if (model != nullptr)
        model->removeChangeListener (this);
}

void ShizukuPluginEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff20242b));
}

void ShizukuPluginEditor::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto top = area.removeFromTop (28);
    vocalLabel.setBounds (top.removeFromLeft (80));
    vocalChoice.setBounds (top.removeFromLeft (180));
    top.removeFromLeft (12);
    accompanimentLabel.setBounds (top.removeFromLeft (140));
    accompanimentChoice.setBounds (top.removeFromLeft (180));
    top.removeFromLeft (12);
    analyseButton.setBounds (top.removeFromLeft (90));
    if (documentController == nullptr)
    {
        top = top.withX (area.getX()).withRight (area.getRight());
        captureButton.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (6);
        finishButton.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (12);
        captureRangeLabel.setBounds (top.removeFromLeft (jmax (0, top.getWidth() - 220)));
    }
    keyLabel.setBounds (top);
    area.removeFromTop (4);
    auto controls = area.removeFromTop (26);
    strengthLabel.setBounds (controls.removeFromLeft (64));
    strengthSlider.setBounds (controls.removeFromLeft (220));
    controls.removeFromLeft (12);
    thresholdLabel.setBounds (controls.removeFromLeft (70));
    thresholdSlider.setBounds (controls.removeFromLeft (220));
    controls.removeFromLeft (12);
    referenceButton.setBounds (controls.removeFromLeft (120));
    controls.removeFromLeft (4);
    clearReferenceButton.setBounds (controls.removeFromLeft (50));
    area.removeFromTop (4);
    auto referenceRow = area.removeFromTop (24);
    referenceChoiceLabel.setBounds (referenceRow.removeFromLeft (110));
    referenceChoice.setBounds (referenceRow.removeFromLeft (420));
    referenceRow.removeFromLeft (8);
    referenceLabel.setBounds (referenceRow);
    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (4);
    pitchLineView.setBounds (area);
}

void ShizukuPluginEditor::changeListenerCallback (ChangeBroadcaster*)
{
    if (documentController != nullptr)
        refreshTrackChoices();
    else
        refreshCapture();
    refreshResult();
}

void ShizukuPluginEditor::refreshTrackChoices()
{
    trackChoices = documentController->getTracks();
    for (auto* box : { &vocalChoice, &accompanimentChoice })
    {
        box->clear (dontSendNotification);
        box->addItem ("(none)", noTrackId);
        for (size_t i = 0; i < trackChoices.size(); ++i)
        {
            auto name = ShizukuDocumentController::getTrackName (*trackChoices[i]);
            if (box == &accompanimentChoice && ShizukuDocumentController::looksLikeAccompaniment (name))
                name += String::fromUTF8 (u8"  (伴奏候補)");
            box->addItem (name, (int) i + 2);
        }
    }

    const auto idFor = [this] (ARARegionSequence* track)
    {
        const auto it = std::find (trackChoices.begin(), trackChoices.end(), track);
        return it == trackChoices.end() ? noTrackId : (int) (it - trackChoices.begin()) + 2;
    };
    vocalChoice.setSelectedId (idFor (documentController->getVocalTrack()), dontSendNotification);
    accompanimentChoice.setSelectedId (idFor (documentController->getAccompanimentTrack()), dontSendNotification);
    analyseButton.setEnabled (! documentController->isAnalysing());
}

void ShizukuPluginEditor::applySliderSettings()
{
    if (model == nullptr)
        return;
    auto settings = model->getCorrectionSettings();
    settings.strength = strengthSlider.getValue() / 100.0;
    settings.thresholdCents = thresholdSlider.getValue();
    model->setCorrectionSettings (settings);
}

void ShizukuPluginEditor::refreshReferenceChoices()
{
    referenceChoice.clear (dontSendNotification);
    const auto candidates = model->getReferenceCandidates();
    const int choice = model->getReferenceChoice();
    String autoText = String::fromUTF8 (u8"自動（区間ごとに照合）");
    if (choice == referenceAuto && ! candidates.empty())
    {
        // Name the candidates the automatic choice actually used, most sung time first.
        std::vector<std::pair<double, String>> used;
        for (const auto& info : candidates)
            if (info.usedSeconds > 0.0)
                used.push_back ({ info.usedSeconds, info.name + (info.shiftedSeconds > 0.0 ? String::fromUTF8 (u8" (拍ずらし ") + String (info.shiftedSeconds, 1) + " s)" : String()) });
        std::sort (used.begin(), used.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        StringArray names;
        for (const auto& [seconds, name] : used)
            names.add (name + " " + String (seconds, 1) + " s");
        if (! names.isEmpty())
            autoText << ": " << names.joinIntoString (", ");
    }
    referenceChoice.addItem (autoText, 1);
    referenceChoice.addItem (String::fromUTF8 (u8"なし（キーの音階）"), 2);
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        const auto& info = candidates[c];
        referenceChoice.addItem (info.name + "  (match " + String (roundToInt (info.looseMatch * 100.0)) + "%, present "
                                     + String (roundToInt (info.presence * 100.0)) + "%, offset " + String (info.offsetSeconds, 2) + " s)",
                                 (int) c + 3);
    }
    referenceChoice.setSelectedId (choice + 3, dontSendNotification);
    referenceChoice.setEnabled (model->canChooseReference());
}

void ShizukuPluginEditor::refreshResult()
{
    refreshReferenceChoices();
    statusLabel.setText (model->getStatusText(), dontSendNotification);
    referenceLabel.setText (model->getReferenceDescription(), dontSendNotification);
    const auto result = model->getLatestResult();
    pitchLineView.setResult (result);
    pitchLineView.setCorrection (model->getLatestCorrection(), model->getLatestTargets());
    if (result != nullptr && result->analysis.hasAccompaniment)
    {
        String text = (documentController == nullptr ? String::fromUTF8 (u8"キー: ") : String ("Key: ")) + String (keyName (result->analysis.key.globalKey));
        if (result->analysis.key.segments.size() > 1)
            text << " (" << (int) result->analysis.key.segments.size() << (documentController == nullptr ? String::fromUTF8 (u8" 区間)") : String (" sections)"));
        keyLabel.setText (text, dontSendNotification);
    }
    else
    {
        keyLabel.setText (documentController == nullptr ? String::fromUTF8 (u8"キー: -") : String ("Key: -"), dontSendNotification);
    }
}

} // namespace shizuku
