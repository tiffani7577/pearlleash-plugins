cmake_minimum_required(VERSION 3.22)
project(Harmonic2K VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Bundle JUCE's JS frontend library into the UI so parameter binding works inside the WebView
file(COPY ${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/ui/js/juce)

juce_add_plugin(Harmonic2K
    COMPANY_NAME "PearlLeashPlugin"
    PLUGIN_MANUFACTURER_CODE Prll
    PLUGIN_CODE PHar
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Harmonic2K"

    NEEDS_WEBVIEW2 TRUE)

file(GLOB_RECURSE UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*)
    file(GLOB_RECURSE ASSET_FILES "BinaryData/*.png")
    juce_add_binary_data(Harmonic2KAssets SOURCES ${UI_FILES} ${ASSET_FILES})

target_sources(Harmonic2K PRIVATE Source/PluginProcessor.cpp)

# offline render test — the agent's ears (build with: cmake --build build --target Harmonic2KTest)
juce_add_console_app(Harmonic2KTest PRODUCT_NAME "Harmonic2KTest")
target_sources(Harmonic2KTest PRIVATE Test/RenderTest.cpp Source/PluginProcessor.cpp)
target_link_libraries(Harmonic2KTest PRIVATE Harmonic2KAssets juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography)
target_compile_definitions(Harmonic2K PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Harmonic2K PRIVATE
    Harmonic2KAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_cryptography
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
