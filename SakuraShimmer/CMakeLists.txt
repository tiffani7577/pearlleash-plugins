cmake_minimum_required(VERSION 3.22)
project(SakuraShimmer VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Bundle JUCE's JS frontend library into the UI so parameter binding works inside the WebView
file(COPY ${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/ui/js/juce)

juce_add_plugin(SakuraShimmer
    COMPANY_NAME "PearlLeashPlugin"
    PLUGIN_MANUFACTURER_CODE Prll
    PLUGIN_CODE jGts
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "SakuraShimmer"

    NEEDS_WEBVIEW2 TRUE)

file(GLOB_RECURSE UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*)
    file(GLOB_RECURSE ASSET_FILES "BinaryData/*.png" "BinaryData/*.jpg" "BinaryData/*.jpeg" "BinaryData/*.webp" "BinaryData/*.gif")
    juce_add_binary_data(SakuraShimmerAssets SOURCES ${UI_FILES} ${ASSET_FILES})

target_sources(SakuraShimmer PRIVATE Source/PluginProcessor.cpp
    Source/WoManusAnalyzer.cpp)

# offline render test — the agent's ears (build with: cmake --build build --target SakuraShimmerTest)
juce_add_console_app(SakuraShimmerTest PRODUCT_NAME "SakuraShimmerTest")
target_sources(SakuraShimmerTest PRIVATE Test/RenderTest.cpp Source/PluginProcessor.cpp
    Source/WoManusAnalyzer.cpp)
target_link_libraries(SakuraShimmerTest PRIVATE SakuraShimmerAssets juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography)
target_compile_definitions(SakuraShimmer PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SakuraShimmer PRIVATE
    SakuraShimmerAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_cryptography
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Linux needs GTK/WebKit/Curl flags propagated to the JUCE modules we compile.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JUCE_LINUX_DEPS REQUIRED gtk+-3.0 webkit2gtk-4.1 libcurl)
    target_include_directories(SakuraShimmer PRIVATE ${JUCE_LINUX_DEPS_INCLUDE_DIRS})
    target_compile_options(SakuraShimmer PRIVATE ${JUCE_LINUX_DEPS_CFLAGS_OTHER})
    target_link_libraries(SakuraShimmer PRIVATE ${JUCE_LINUX_DEPS_LIBRARIES})
    target_include_directories(SakuraShimmerTest PRIVATE ${JUCE_LINUX_DEPS_INCLUDE_DIRS})
    target_compile_options(SakuraShimmerTest PRIVATE ${JUCE_LINUX_DEPS_CFLAGS_OTHER})
    target_link_libraries(SakuraShimmerTest PRIVATE ${JUCE_LINUX_DEPS_LIBRARIES})
endif()
