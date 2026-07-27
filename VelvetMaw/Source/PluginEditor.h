#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "BinaryData.h"
#include "AssetRenderer.h"




class VelvetMawLookAndFeel : public juce::LookAndFeel_V4 {
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

class VelvetMawEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VelvetMawEditor (VelvetMawProcessor& p) : AudioProcessorEditor (p), proc (p)
    {
        loadAssets();
        cutoffAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("cutoff"), cutoffRelay, nullptr);
        resonanceAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("resonance"), resonanceRelay, nullptr);
        predriveAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("predrive"), predriveRelay, nullptr);
        postdriveAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("postdrive"), postdriveRelay, nullptr);
        gainAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("gain"), gainRelay, nullptr);
        mixAttach = std::make_unique<juce::WebSliderParameterAttachment> (*proc.apvts.getParameter ("mix"), mixRelay, nullptr);
        addAndMakeVisible (webView);
        webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());



        setSize (600, 400);
        startTimerHz (30);
    }

    void paint(juce::Graphics& g) override {
        if (bgAtlas.sheet.isValid()) {
            g.drawImage(bgAtlas.sheet, getLocalBounds().toFloat());
        }
    }

    void loadAssets() {
        // No BinaryData atlases in this export — WebView UI only.
        juce::ignoreUnused (lnf);
    }

    void resized() override {
        webView.setBounds (getLocalBounds());



    }
    void timerCallback() override
    {
        const float rms = proc.outputRmsLevel.load();
        const float meterDb = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, rms), -100.0f);
        webView.evaluateJavascript ("window.postMessage({type:'meter',level:" + juce::String (meterDb, 2) + "},'*');");
        webView.evaluateJavascript (
            "window.postMessage({type:'spectrum',pre:" + proc.analyzer.getPreEQMagnitudesJsArray()
            + ",post:" + proc.analyzer.getPostEQMagnitudesJsArray() + "},'*');");
        {
        juce::String eqJs = "window.postMessage({type:'eq',bands:[";
        
        eqJs += "{freq:" + juce::String (proc.apvts.getRawParameterValue ("cutoff")->load(), 2)
               + ",gain:" + juce::String (0.0f, 2)
               + ",q:" + juce::String (juce::jmax (0.1f, proc.apvts.getRawParameterValue ("resonance")->load() * 10.0f), 3) + "}";
        eqJs += "]},'*');";
        webView.evaluateJavascript (eqJs);
    }

    }

private:
    VelvetMawProcessor& proc;
    VelvetMawLookAndFeel lnf;
    VelvetMawLookAndFeel::SpriteAtlas bgAtlas;
    juce::WebSliderRelay cutoffRelay { "cutoff" };
    juce::WebSliderRelay resonanceRelay { "resonance" };
    juce::WebSliderRelay predriveRelay { "predrive" };
    juce::WebSliderRelay postdriveRelay { "postdrive" };
    juce::WebSliderRelay gainRelay { "gain" };
    juce::WebSliderRelay mixRelay { "mix" };




    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url)
    {
        // Studio assets referenced as /api/plugforge/assets/{uuid} are packaged
        // into BinaryData/{uuid}.png (etc.) at export time.
        if (url.startsWith ("/api/plugforge/assets/"))
        {
            const auto id = url.fromLastOccurrenceOf ("/", false, false);
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

        const auto file = url == "/" ? juce::String ("index.html") : url.fromLastOccurrenceOf ("/", false, false);
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            if (juce::String (BinaryData::getNamedResourceOriginalFilename (BinaryData::namedResourceList[i])) == file)
            {
                int size = 0;
                const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                const auto mime = file.endsWith (".js") ? "text/javascript"
                                : file.endsWith (".css") ? "text/css" : "text/html";
                return juce::WebBrowserComponent::Resource {
                    std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                            reinterpret_cast<const std::byte*> (data) + size),
                    mime };
            }
        }
        return std::nullopt;
    }

    juce::WebBrowserComponent webView {
        juce::WebBrowserComponent::Options{}
            .withBackend (juce::WebBrowserComponent::Options::Backend::defaultBackend)
            .withNativeIntegrationEnabled()
            .withOptionsFrom (cutoffRelay)
            .withOptionsFrom (resonanceRelay)
            .withOptionsFrom (predriveRelay)
            .withOptionsFrom (postdriveRelay)
            .withOptionsFrom (gainRelay)
            .withOptionsFrom (mixRelay)
            .withResourceProvider ([] (const auto& url) { return getResource (url); })
    };

    std::unique_ptr<juce::WebSliderParameterAttachment> cutoffAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> resonanceAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> predriveAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> postdriveAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> gainAttach;
    std::unique_ptr<juce::WebSliderParameterAttachment> mixAttach;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelvetMawEditor)
};
