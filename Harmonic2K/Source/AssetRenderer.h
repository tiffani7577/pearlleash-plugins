#pragma once
#include <JuceHeader.h>

struct SpriteAtlas {
    juce::Image image;
    int numFrames;
    int frameWidth;
    int frameHeight;

    void drawFrame(juce::Graphics& g, int frameIndex, int x, int y, int w, int h) {
        if (!image.isValid()) return;
        frameIndex = juce::jlimit(0, numFrames - 1, frameIndex);
        g.drawImage(image, x, y, w, h, 0, frameIndex * frameHeight, frameWidth, frameHeight);
    }
};

class AssetRenderer {
public:
    static void drawActor(juce::Graphics& g, const juce::String& actorId, float value, int x, int y, int w, int h) {
        auto atlas = getAtlas(actorId);
        if (atlas) {
            int frame = (int)(value * (atlas->numFrames - 1));
            atlas->drawFrame(g, frame, x, y, w, h);
        } else {
            // Fallback drawing
            g.setColour(juce::Colours::grey);
            g.drawRect(x, y, w, h);
        }
    }

private:
    static SpriteAtlas* getAtlas(const juce::String& id) {
        static std::map<juce::String, SpriteAtlas> cache;
        if (cache.count(id)) return &cache[id];
        
        // This will be populated by generate.js with BinaryData calls
        
        
        return cache.count(id) ? &cache[id] : nullptr;
    }
};
