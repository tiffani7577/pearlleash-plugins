cmake_minimum_required(VERSION 3.22)
project(EchoChamber VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Bundle JUCE's JS frontend library into the UI so parameter binding works inside the WebView
file(COPY ${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/ DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/ui/js/juce)

# Default ON for manual Mac builds; agent forces -DPLUGFORGE_COPY_AFTER_BUILD=OFF so
# evaluationOnly candidate shells never auto-install a blank AU into Logic.
option(PLUGFORGE_COPY_AFTER_BUILD "JUCE COPY_PLUGIN_AFTER_BUILD into ~/Library Audio Plug-Ins" ON)

juce_add_plugin(EchoChamber
    COMPANY_NAME "PearlLeashPlugin"
    PLUGIN_MANUFACTURER_CODE Prll
    PLUGIN_CODE mbnK
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "EchoChamber"
    COPY_PLUGIN_AFTER_BUILD ${PLUGFORGE_COPY_AFTER_BUILD}

    NEEDS_WEBVIEW2 TRUE)

file(GLOB_RECURSE UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*)
    file(GLOB_RECURSE ASSET_FILES "BinaryData/*.png" "BinaryData/*.jpg" "BinaryData/*.jpeg" "BinaryData/*.webp" "BinaryData/*.gif")
    juce_add_binary_data(EchoChamberAssets SOURCES ${UI_FILES} ${ASSET_FILES})

target_sources(EchoChamber PRIVATE Source/PluginProcessor.cpp
    Source/WoManusAnalyzer.cpp)

# offline render test — the agent's ears (build with: cmake --build build --target EchoChamberTest)
juce_add_console_app(EchoChamberTest PRODUCT_NAME "EchoChamberTest")
target_sources(EchoChamberTest PRIVATE Test/RenderTest.cpp Source/PluginProcessor.cpp
    Source/WoManusAnalyzer.cpp)
target_link_libraries(EchoChamberTest PRIVATE EchoChamberAssets juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography)
target_compile_definitions(EchoChamber PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    RTNEURAL_DEFAULT_ALIGNMENT=16)
target_include_directories(EchoChamber PRIVATE
    ${CMAKE_SOURCE_DIR}/../../builtins/EliteBlocks
    ${CMAKE_SOURCE_DIR}/../../builtins/EliteBlocks/RTNeural)
target_include_directories(EchoChamberTest PRIVATE
    ${CMAKE_SOURCE_DIR}/../../builtins/EliteBlocks
    ${CMAKE_SOURCE_DIR}/../../builtins/EliteBlocks/RTNeural)
target_link_libraries(EchoChamber PRIVATE
    EchoChamberAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_cryptography
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Linux needs GTK/WebKit/Curl flags propagated to the JUCE modules we compile.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JUCE_LINUX_DEPS REQUIRED gtk+-3.0 webkit2gtk-4.1 libcurl)
    target_include_directories(EchoChamber PRIVATE ${JUCE_LINUX_DEPS_INCLUDE_DIRS})
    target_compile_options(EchoChamber PRIVATE ${JUCE_LINUX_DEPS_CFLAGS_OTHER})
    target_link_libraries(EchoChamber PRIVATE ${JUCE_LINUX_DEPS_LIBRARIES})
    target_include_directories(EchoChamberTest PRIVATE ${JUCE_LINUX_DEPS_INCLUDE_DIRS})
    target_compile_options(EchoChamberTest PRIVATE ${JUCE_LINUX_DEPS_CFLAGS_OTHER})
    target_link_libraries(EchoChamberTest PRIVATE ${JUCE_LINUX_DEPS_LIBRARIES})
endif()

# CLAP via clap-juce-extensions (optional — AU/VST3 must never die if CLAP is missing).
# Network FetchContent is OFF by default: FetchContent_Populate/MakeAvailable still
# FATAL_ERROR on offline Macs and was a real cmake exit-65 forge killer (see #182).
# Enable explicitly: cmake -DPLUGFORGE_ENABLE_CLAP=ON -DPLUGFORGE_FETCH_CLAP=ON
option(PLUGFORGE_ENABLE_CLAP "Build CLAP when clap-juce-extensions is available" OFF)
option(PLUGFORGE_FETCH_CLAP "Allow network FetchContent for clap-juce-extensions" OFF)
if(PLUGFORGE_ENABLE_CLAP)
    find_package(clap-juce-extensions QUIET)
    if(NOT clap-juce-extensions_FOUND AND PLUGFORGE_FETCH_CLAP)
        include(FetchContent)
        FetchContent_Declare(clap-juce-extensions
            GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE)
        FetchContent_GetProperties(clap-juce-extensions)
        if(NOT clap-juce-extensions_POPULATED)
            FetchContent_Populate(clap-juce-extensions)
        endif()
        if(clap-juce-extensions_POPULATED AND EXISTS "${clap-juce-extensions_SOURCE_DIR}/CMakeLists.txt")
            add_subdirectory("${clap-juce-extensions_SOURCE_DIR}" "${clap-juce-extensions_BINARY_DIR}" EXCLUDE_FROM_ALL)
        else()
            message(STATUS "[PlugForge] clap-juce-extensions fetch failed — CLAP skipped (AU/VST3 unaffected)")
        endif()
    endif()
    if(TARGET EchoChamber AND COMMAND clap_juce_extensions_plugin)
        clap_juce_extensions_plugin(
            TARGET EchoChamber
            CLAP_ID "com.pearlleash.echochamber"
            CLAP_FEATURES "audio-effect" "stereo" "delay"
        )
    else()
        message(STATUS "[PlugForge] clap_juce_extensions_plugin not available — CLAP skipped (AU/VST3 unaffected)")
    endif()
else()
    message(STATUS "[PlugForge] PLUGFORGE_ENABLE_CLAP=OFF — CLAP skipped (AU/VST3 unaffected)")
endif()

# macOS: after AU build, install + refresh AU cache (Logic-friendly).
# NEVER run auval inside POST_BUILD — a FAIL would make cmake --build exit 65
# with no compiler errors (looks like a mysterious compile failure).
# Build EchoChamber.command / the local agent run auval after compile as a separate gate.
# Override with -DPLUGFORGE_DEV_INSTALL=OFF for agent / CI package-only builds.
option(PLUGFORGE_DEV_INSTALL "Install AU/VST3/CLAP + refresh AU cache after build" ON)
if(APPLE AND PLUGFORGE_DEV_INSTALL)
    set(_pf_install_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos-dev-install.sh")
    if(EXISTS "${_pf_install_script}")
        add_custom_command(TARGET EchoChamber_AU POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env
                PLUGIN_NAME=EchoChamber
                BUILD_DIR=${CMAKE_BINARY_DIR}
                CONFIG=$<CONFIG>
                AU_SUBTYPE=mbnK
                AU_MFR=Prll
                AU_TYPE=aufx
                SKIP_AUVAL=1
                bash "${_pf_install_script}"
            COMMENT "PlugForge: install plugins + refresh AU cache (auval deferred)"
            VERBATIM)
    endif()
endif()
