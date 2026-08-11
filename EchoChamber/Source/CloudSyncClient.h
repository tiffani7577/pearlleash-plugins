#pragma once
#include <juce_core/juce_core.h>
#include <atomic>

// WoManus Builtin Block: cloud.sync_client
// Category: cloud, connectivity, updates
// When to use: Every plugin gets this automatically
// What it does: Background sync of presets, IR updates,
//   model weights, and license verification
// Never forget: All DSP runs locally. Cloud only
//   provides data updates. Plugin works offline forever.
// CPU cost: zero (background thread only — never on audio thread)

#ifndef PEARLLEASH_API_ENDPOINT
#define PEARLLEASH_API_ENDPOINT "https://api.pearlleash.com"
#endif

/**
 * Living-plugin cloud client. Optional connectivity:
 * - Offline: no-op; DSP and presets work forever from disk
 * - Online: check updates, fetch community presets, upload user presets
 * Never streams audio. Never blocks processBlock.
 */
class CloudSyncClient : private juce::Thread {
public:
  struct SyncPayload {
    juce::String pluginId;
    juce::String licenseKey;
    juce::String version;
    juce::String platform;
  };

  CloudSyncClient() : juce::Thread ("CloudSyncClient") {}

  ~CloudSyncClient() override {
    stopSync();
  }

  void prepare(const juce::String& pluginId,
               const juce::String& licenseKey,
               const juce::String& version) noexcept {
    const juce::ScopedLock sl (lock);
    payload.pluginId = pluginId;
    payload.licenseKey = licenseKey;
    payload.version = version;
    payload.platform = juce::SystemStats::getOperatingSystemName();
  }

  /** Set API base (default PEARLLEASH_API_ENDPOINT). Empty = sync disabled. */
  void setApiBase(const juce::String& base) noexcept {
    const juce::ScopedLock sl (lock);
    apiBase = base.trim().isEmpty()
      ? juce::String (PEARLLEASH_API_ENDPOINT)
      : base.trim().trimCharactersAtEnd ("/");
  }

  // Call once on plugin startup (background thread)
  void startSync() {
    if (isThreadRunning()) return;
    shouldStop.store (false);
    startThread (juce::Thread::Priority::background);
  }

  // Returns true if new content available
  bool hasUpdates() const noexcept {
    return updatesAvailable.load();
  }

  // Get update notification for UI
  juce::String getUpdateMessage() const noexcept {
    const juce::ScopedLock sl (lock);
    return updateMessage;
  }

  void stopSync() noexcept {
    shouldStop.store (true);
    signalThreadShouldExit();
    // Short join — auval constructs/destroys many instances; a 2s block marks AUs unstable.
    stopThread (200);
  }

private:
  void run() override {
    checkForUpdates();
    if (threadShouldExit() || shouldStop.load()) return;
    fetchCommunityPresets();
    if (threadShouldExit() || shouldStop.load()) return;
    uploadUserPresets();
  }

  juce::String baseUrl() const {
    const juce::ScopedLock sl (lock);
    return apiBase;
  }

  SyncPayload copyPayload() const {
    const juce::ScopedLock sl (lock);
    return payload;
  }

  void setUpdateNotice(const juce::String& msg) {
    {
      const juce::ScopedLock sl (lock);
      updateMessage = msg;
    }
    updatesAvailable.store (true);
  }

  void checkForUpdates() {
    if (shouldStop.load() || threadShouldExit()) return;
    const auto p = copyPayload();
    if (p.pluginId.isEmpty()) return;

    // POST to WoManus cloud API — fail soft (offline forever)
    try {
      juce::URL url (baseUrl() + "/api/plugins/check-updates");
      auto* body = new juce::DynamicObject();
      body->setProperty ("pluginId", p.pluginId);
      body->setProperty ("version", p.version);
      body->setProperty ("licenseKey", p.licenseKey);
      body->setProperty ("platform", p.platform);
      const auto json = juce::JSON::toString (juce::var (body));
      auto postUrl = url.withPOSTData (json);
      auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                      .withConnectionTimeoutMs (4000)
                      .withExtraHeaders ("Content-Type: application/json\r\n");
      std::unique_ptr<juce::InputStream> stream (postUrl.createInputStream (opts));
      if (stream == nullptr) return;
      const auto response = stream->readEntireStreamAsString();
      auto parsed = juce::JSON::parse (response);
      if (! parsed.isObject()) return;
      const bool hasUpdate = (bool) parsed.getProperty ("hasUpdate", false);
      if (! hasUpdate) return;
      juce::StringArray parts;
      const int newPresets = (int) parsed.getProperty ("newPresets", 0);
      const int newIRs = (int) parsed.getProperty ("newIRs", 0);
      const auto changelog = parsed.getProperty ("changelog", {}).toString();
      // What's New badge — factory + community preset packs
      if (newPresets > 0)
      {
        if (changelog.containsIgnoreCase ("factory preset pack")
            || changelog.containsIgnoreCase ("What's New"))
          parts.add ("New preset pack available (" + juce::String (newPresets) + ")");
        else
          parts.add (juce::String (newPresets) + " new community presets available");
      }
      if (newIRs > 0)
        parts.add ("New IR pack ready to download");
      if (changelog.isNotEmpty() && ! parts.joinIntoString (" ").contains (changelog))
        parts.add (changelog);
      if (parts.isEmpty())
        parts.add ("Updates available for this plugin");
      setUpdateNotice (parts.joinIntoString (" · "));
    } catch (...) {
      // Offline / network error — plugin keeps working locally
    }
  }

  void fetchCommunityPresets() {
    if (shouldStop.load() || threadShouldExit()) return;
    const auto p = copyPayload();
    if (p.pluginId.isEmpty()) return;
    try {
      juce::URL url (baseUrl() + "/api/plugins/presets/community"
                     + "?pluginId=" + juce::URL::addEscapeChars (p.pluginId, true)
                     + "&limit=40&sort=rating");
      auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                      .withConnectionTimeoutMs (4000);
      std::unique_ptr<juce::InputStream> stream (url.createInputStream (opts));
      if (stream == nullptr) return;
      const auto response = stream->readEntireStreamAsString();
      auto parsed = juce::JSON::parse (response);
      // Persist under Application Support when array present — soft fail if empty
      if (! parsed.isArray() || parsed.size() == 0) return;
      auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("PearlLeash")
                     .getChildFile (p.pluginId.replaceCharacter ('.', '_'))
                     .getChildFile ("community-presets");
      dir.createDirectory();
      dir.getChildFile ("latest.json").replaceWithText (response);
      // Also materialize individual JSON / clap-preset files for the host browser
      for (int i = 0; i < parsed.size(); ++i)
      {
        auto item = parsed[i];
        if (! item.isObject()) continue;
        const auto presetName = item.getProperty ("presetName", "preset").toString();
        const auto presetData = item.getProperty ("presetData", "").toString();
        if (presetData.isEmpty()) continue;
        auto safe = presetName.replaceCharacters (" /\\:", "----");
        dir.getChildFile (safe + ".json").replaceWithText (presetData);
        if (presetData.contains ("\"clap-preset\""))
          dir.getChildFile (safe + ".clap-preset").replaceWithText (presetData);
      }
    } catch (...) {
    }
  }

  void uploadUserPresets() {
    if (shouldStop.load() || threadShouldExit()) return;
    // Opt-in upload of local custom presets — JSON + .clap-preset
    const auto p = copyPayload();
    if (p.pluginId.isEmpty() || p.licenseKey.isEmpty()) return;
    try {
      auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("PearlLeash")
                     .getChildFile (p.pluginId.replaceCharacter ('.', '_'))
                     .getChildFile ("user-presets");
      if (! dir.isDirectory()) return;
      auto uploadOne = [this, &p] (const juce::File& f)
      {
        if (shouldStop.load() || threadShouldExit()) return;
        auto* body = new juce::DynamicObject();
        body->setProperty ("pluginId", p.pluginId);
        body->setProperty ("presetName", f.getFileNameWithoutExtension());
        body->setProperty ("presetData", f.loadFileAsString());
        body->setProperty ("anonymous", true);
        body->setProperty ("licenseKey", p.licenseKey);
        const auto json = juce::JSON::toString (juce::var (body));
        juce::URL url (baseUrl() + "/api/plugins/presets/upload");
        auto postUrl = url.withPOSTData (json);
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                        .withConnectionTimeoutMs (4000)
                        .withExtraHeaders ("Content-Type: application/json\r\n");
        std::unique_ptr<juce::InputStream> stream (postUrl.createInputStream (opts));
        juce::ignoreUnused (stream);
      };
      for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
        uploadOne (f);
      for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.clap-preset"))
        uploadOne (f);
    } catch (...) {
    }
  }

  SyncPayload payload;
  juce::String apiBase { PEARLLEASH_API_ENDPOINT };
  juce::String updateMessage;
  mutable juce::CriticalSection lock;
  std::atomic<bool> shouldStop { false };
  std::atomic<bool> updatesAvailable { false };
};
