#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "BinaryData.h"
#include "AssetRenderer.h"
#include "AudioReactiveKnob.h"






class EchoChamberLookAndFeel : public juce::LookAndFeel_V4 {
public:
    struct SpriteAtlas {
        juce::Image sheet;
        int frameCount = 1;
        int frameSize = 128;

        juce::Image getFrameForValue (float sliderPos) const {
            if (! sheet.isValid() || frameCount <= 0) return {};
            const int idx = juce::jlimit (0, frameCount - 1,
                (int) std::round (sliderPos * (float) (frameCount - 1)));
            return sheet.getClippedImage (juce::Rectangle<int> (0, idx * frameSize, frameSize, frameSize));
        }

        static SpriteAtlas loadFromBinary (const char* data, int size, int frames, int fSize = 128) {
            SpriteAtlas a;
            a.sheet = juce::ImageCache::getFromMemory (data, (size_t) size);
            a.frameCount = frames;
            a.frameSize = fSize;
            return a;
        }
    };

    std::map<std::string, SpriteAtlas> atlases;
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override {
        auto it = atlases.find(slider.getName().toStdString());
        if (it != atlases.end()) {
            auto frame = it->second.getFrameForValue(sliderPos);
            if (frame.isValid())
                g.drawImage(frame, juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height));
            else
                juce::LookAndFeel_V4::drawRotarySlider(g, x, y, width, height, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
        } else {
            juce::LookAndFeel_V4::drawRotarySlider(g, x, y, width, height, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
        }
    }
};

class EchoChamberEditor : public juce::AudioProcessorEditor,
                      private juce::Timer,
                      private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit EchoChamberEditor (EchoChamberProcessor& p)
        : juce::AudioProcessorEditor (p),
          proc (p),
          webView (makeWebViewOptions())
    {
        loadAssets();
        roomSizeAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("roomSize"), roomSizeRelay, nullptr);
        gainAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("gain"), gainRelay, nullptr);
        bypassAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("bypass"), bypassRelay, nullptr);
        widthAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("width"), widthRelay, nullptr);
        decorrelateAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("decorrelate"), decorrelateRelay, nullptr);
        gainSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        gainSlider.setLookAndFeel (&knobLAF);
        nativeGainSliderAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "gain", gainSlider);
        addAndMakeVisible (webView);
        addAndMakeVisible (gainSlider);



        setSize (520, 340);
        setResizeLimits (400, 300, 1200, 900);
        setResizable (true, true);
        proc.apvts.addParameterListener ("roomSize", this);
        proc.apvts.addParameterListener ("gain", this);
        proc.apvts.addParameterListener ("bypass", this);
        proc.apvts.addParameterListener ("width", this);
        proc.apvts.addParameterListener ("decorrelate", this);
        // AU/Logic stability: never call goToURL / startTimer / evaluateJavascript
        // synchronously in the constructor. Schedule bootstrap on the next message-thread
        // tick so createEditor() can return first — Logic often never fires a reliable
        // visibilityChanged, which previously left the WebView permanently blank.
        juce::Component::SafePointer<EchoChamberEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]()
        {
            if (safeThis != nullptr)
                safeThis->bootstrapUiIfNeeded();
        });
    }

    ~EchoChamberEditor() override
    {
        stopTimer();
        proc.apvts.removeParameterListener ("roomSize", this);
        proc.apvts.removeParameterListener ("gain", this);
        proc.apvts.removeParameterListener ("bypass", this);
        proc.apvts.removeParameterListener ("width", this);
        proc.apvts.removeParameterListener ("decorrelate", this);
        gainSlider.setLookAndFeel (nullptr);
    }

    void visibilityChanged() override
    {
        if (isVisible())
            bootstrapUiIfNeeded();
    }

    void parentHierarchyChanged() override
    {
        if (getParentComponent() != nullptr && isShowing())
            bootstrapUiIfNeeded();
    }

    /** Load WebView HTML once the editor exists on the message thread. */
    void bootstrapUiIfNeeded()
    {
        if (uiBootstrapped)
            return;
        uiBootstrapped = true;
        webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
        startTimerHz (30);
        // Push params after the page has a chance to install JS listeners.
        juce::Component::SafePointer<EchoChamberEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]()
        {
            if (safeThis != nullptr)
                safeThis->pushAllParametersToWebView();
        });
        proc.cloudSync.startSync();
    }

    /** C++ → JS: push parameter JSON via evaluateJavascript (WebView bridge). */
    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        juce::ignoreUnused (newValue);
        auto* param = proc.apvts.getParameter (parameterID);
        if (param == nullptr) return;
        const float norm = param->getValue();
        const juce::String json = "{\"id\":\"" + parameterID + "\",\"value\":" + juce::String (norm, 6) + "}";
        webView.evaluateJavascript (
            "window.dispatchEvent(new CustomEvent('plugforge-param',{detail:" + json + "}));"
            "if(window.__plugforgeSetParam)window.__plugforgeSetParam(" + json + ");");
    }

    void pushAllParametersToWebView()
    {
        parameterChanged ("roomSize", 0.0f);
        parameterChanged ("gain", 0.0f);
        parameterChanged ("bypass", 0.0f);
        parameterChanged ("width", 0.0f);
        parameterChanged ("decorrelate", 0.0f);
    }

    void paint(juce::Graphics& g) override {
        if (bgAtlas.sheet.isValid()) {
            g.drawImage(bgAtlas.sheet, getLocalBounds().toFloat());
        } else {
            g.fillAll (juce::Colour (0xff1a1a1a));
        }
        // Native identity when WebView has not painted yet (common Logic blank-AU case).
        // Corner/centre gainSlider remains the always-on control.
        if (! uiBootstrapped)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (22.0f);
            g.drawText ("EchoChamber", getLocalBounds().removeFromTop (48), juce::Justification::centred, true);
            g.setFont (13.0f);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawFittedText ("Loading UI… use the centre knob if the page stays blank.\nExport to DAW from PlugForge after updating the Mac agent.",
                getLocalBounds().reduced (24), juce::Justification::centred, 3);
        }
    }

    void loadAssets() {
        // No BinaryData atlases in this export — WebView UI only.
        juce::ignoreUnused (lnf);
    }

    void resized() override {
        auto bounds = getLocalBounds();
        webView.setBounds (bounds);
        // Large native safety knob — always usable if WebView stays blank in Logic.
        const int knob = juce::jlimit (64, 96, juce::jmin (bounds.getWidth(), bounds.getHeight()) / 5);
        gainSlider.setBounds (bounds.getCentreX() - knob / 2, bounds.getBottom() - knob - 16, knob, knob);
        gainSlider.toFront (false);



        // Fallback if callAsync was dropped before the editor was parented (some AU hosts).
        if (bounds.getWidth() > 0 && bounds.getHeight() > 0)
            bootstrapUiIfNeeded();
    }
    void timerCallback() override
    {
        if (! uiBootstrapped)
            return;
        const float rms = proc.outputRmsLevel.load();
        const float meterDb = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, rms), -100.0f);
        knobLAF.setLevel (meterDb);
        gainSlider.repaint();


        webView.evaluateJavascript ("window.postMessage({type:'meter',level:" + juce::String (meterDb, 2) + "},'*');");
        webView.evaluateJavascript (
            "window.postMessage({type:'spectrum',pre:" + proc.analyzer.getPreEQMagnitudesJsArray()
            + ",post:" + proc.analyzer.getPostEQMagnitudesJsArray() + "},'*');");
        webView.evaluateJavascript ("window.postMessage({type:'eq',bands:[]},'*');");
        if (proc.cloudSync.hasUpdates() && ! cloudUpdatePushed)
        {
            cloudUpdatePushed = true;
            const juce::String msgJson = juce::JSON::toString (juce::var (proc.cloudSync.getUpdateMessage()));
            webView.evaluateJavascript (
                "window.postMessage({type:'cloudUpdate',message:" + msgJson + "},'*');");
        }

    }

private:
    EchoChamberProcessor& proc;


    AudioReactiveKnob knobLAF;
    juce::Slider gainSlider;
    bool cloudUpdatePushed = false;
    bool uiBootstrapped = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> nativeGainSliderAttach;
    EchoChamberLookAndFeel lnf;
    EchoChamberLookAndFeel::SpriteAtlas bgAtlas;
    juce::WebSliderRelay roomSizeRelay { "roomSize" };
    juce::WebSliderRelay gainRelay { "gain" };
    juce::WebSliderRelay bypassRelay { "bypass" };
    juce::WebSliderRelay widthRelay { "width" };
    juce::WebSliderRelay decorrelateRelay { "decorrelate" };




    static juce::String resourceFileFromUrl (const juce::String& url)
    {
        auto path = url.trim();
        // Resource-provider URLs may be "/", "/index.html", or scheme://host/...
        if (path.contains ("://"))
            path = path.fromFirstOccurrenceOf ("://", false, false)
                       .fromFirstOccurrenceOf ("/", false, false);
        while (path.startsWithChar ('/'))
            path = path.substring (1);
        path = path.upToFirstOccurrenceOf ("?", false, false)
                   .upToFirstOccurrenceOf ("#", false, false);
        if (path.isEmpty() || path.equalsIgnoreCase ("index.html"))
            return "index.html";
        // BinaryData original filenames are basenames from the ui/ tree.
        return path.fromLastOccurrenceOf ("/", false, false);
    }

    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url)
    {
        // Studio assets referenced as /api/plugforge/assets/{uuid} are packaged
        // into BinaryData/{uuid}.png (etc.) at export time.
        if (url.containsIgnoreCase ("/api/plugforge/assets/"))
        {
            const auto id = url.fromLastOccurrenceOf ("/", false, false)
                               .upToFirstOccurrenceOf ("?", false, false);
            if (id.isNotEmpty())
            {
                const juce::StringArray candidates { id + ".png", id + ".jpg", id + ".jpeg", id + ".webp", id + ".gif", id };
                for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
                {
                    const auto filename = juce::String (BinaryData::getNamedResourceOriginalFilename (BinaryData::namedResourceList[i]));
                    if (candidates.contains (filename))
                    {
                        int size = 0;
                        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                        if (data == nullptr || size <= 0)
                            continue;
                        const auto mime = filename.endsWithIgnoreCase (".jpg") || filename.endsWithIgnoreCase (".jpeg") ? "image/jpeg"
                                        : filename.endsWithIgnoreCase (".webp") ? "image/webp"
                                        : filename.endsWithIgnoreCase (".gif") ? "image/gif"
                                        : "image/png";
                        return juce::WebBrowserComponent::Resource {
                            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                                    reinterpret_cast<const std::byte*> (data) + size),
                            mime };
                    }
                }
            }
        }

        const auto file = resourceFileFromUrl (url);
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            if (juce::String (BinaryData::getNamedResourceOriginalFilename (BinaryData::namedResourceList[i])) == file)
            {
                int size = 0;
                const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                if (data == nullptr || size <= 0)
                    continue;
                const auto mime = file.endsWithIgnoreCase (".js") ? "text/javascript"
                                : file.endsWithIgnoreCase (".css") ? "text/css"
                                : file.endsWithIgnoreCase (".svg") ? "image/svg+xml"
                                : file.endsWithIgnoreCase (".json") ? "application/json"
                                : file.endsWithIgnoreCase (".png") ? "image/png"
                                : file.endsWithIgnoreCase (".jpg") || file.endsWithIgnoreCase (".jpeg") ? "image/jpeg"
                                : file.endsWithIgnoreCase (".webp") ? "image/webp"
                                : "text/html";
                return juce::WebBrowserComponent::Resource {
                    std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                            reinterpret_cast<const std::byte*> (data) + size),
                    mime };
            }
        }
        return std::nullopt;
    }

    juce::WebBrowserComponent::Options makeWebViewOptions()
    {
        auto opts = juce::WebBrowserComponent::Options{}
            .withBackend (juce::WebBrowserComponent::Options::Backend::defaultBackend)
            .withNativeIntegrationEnabled()
            // Logic / FL hide the editor without destroying it — default unloads to about:blank.
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withOptionsFrom (roomSizeRelay)
            .withOptionsFrom (gainRelay)
            .withOptionsFrom (bypassRelay)
            .withOptionsFrom (widthRelay)
            .withOptionsFrom (decorrelateRelay)
            .withResourceProvider ([] (const auto& url) { return getResource (url); });

        // UI → C++: JS native "setParam"(id, value) → apvts.getParameter(id)->setValue(...)
        // Normalised domain matches juce::NormalisableRange (same as UI contract skew helpers).
        opts = opts.withNativeFunction ("setParam",
            [this] (const juce::Array<juce::var>& args, auto complete)
            {
                if (args.size() < 2)
                {
                    complete (juce::var());
                    return;
                }
                const auto id = args[0].toString();
                const float value = (float) args[1];
                if (auto* param = proc.apvts.getParameter (id))
                {
                    param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
                }
                else
                {
                    // UI_PARAMETER_NOT_FOUND — never silently no-op.
                   #if JUCE_DEBUG
                    DBG ("UI_PARAMETER_NOT_FOUND: " + id);
                    jassertfalse;
                   #endif
                    juce::Logger::writeToLog ("UI_PARAMETER_NOT_FOUND: " + id);
                }
                complete (juce::var());
            });
        return opts;
    }

    juce::WebBrowserComponent webView;

    std::unique_ptr<juce::WebSliderParameterAttachment> roomSizeAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> gainAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> bypassAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> widthAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> decorrelateAttach;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EchoChamberEditor)
};
