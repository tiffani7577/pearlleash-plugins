#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

struct ActorState {
    float opacity = 1.0f;
    float scale = 1.0f;
    juce::Colour tint = juce::Colours::transparentBlack;
    float glowIntensity = 0.0f;
};

class StateMachine {
public:
    void update(float dt) {
        // Handle transitions and animations
        
    }

    ActorState getCurrentState(const juce::String& actorId) {
        return states[actorId];
    }

private:
    std::map<juce::String, ActorState> states;
    
};
