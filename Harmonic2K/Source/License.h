#pragma once
// Harmonic2K offline license check. Serial = first 20 hex chars of
// HMAC-SHA256(productSecret, lowercase(email)), grouped XXXX-XXXX-XXXX-XXXX-XXXX.
// License file: ~/Library/Application Support/PearlLeashPlugin/Harmonic2K/license.json
// Modes: notice (log-only, default) | strict (silence output when unlicensed).
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace Harmonic2KLicense
{
    static const char* kProductSecret = "3acd8e0f614d8c94692416eaf7377ab4bde1f9fc8acea846e2d8142c573f39af";
    static const char* kMode = "notice";

    inline juce::MemoryBlock hmacSha256 (const juce::MemoryBlock& key, const juce::MemoryBlock& msg)
    {
        constexpr int blockSize = 64;
        juce::MemoryBlock k (key);
        if (k.getSize() > blockSize)
        {
            juce::SHA256 h (k.getData(), k.getSize());
            k = h.getRawData();
        }
        k.setSize (blockSize, true);
        juce::MemoryBlock ipad (blockSize), opad (blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            static_cast<char*> (ipad.getData())[i] = static_cast<const char*> (k.getData())[i] ^ 0x36;
            static_cast<char*> (opad.getData())[i] = static_cast<const char*> (k.getData())[i] ^ 0x5c;
        }
        juce::MemoryBlock inner (ipad); inner.append (msg.getData(), msg.getSize());
        juce::SHA256 innerH (inner.getData(), inner.getSize());
        juce::MemoryBlock outer (opad); outer.append (innerH.getRawData().getData(), innerH.getRawData().getSize());
        juce::SHA256 outerH (outer.getData(), outer.getSize());
        return outerH.getRawData();
    }

    inline juce::String expectedSerial (const juce::String& email)
    {
        auto e = email.trim().toLowerCase();
        juce::MemoryBlock key (kProductSecret, strlen (kProductSecret));
        juce::MemoryBlock msg (e.toRawUTF8(), e.getNumBytesAsUTF8());
        auto raw = hmacSha256 (key, msg);
        auto hex = juce::String::toHexString (raw.getData(), (int) raw.getSize(), 0).substring (0, 20).toUpperCase();
        juce::String out;
        for (int i = 0; i < 20; i += 4)
            out += (i ? "-" : "") + hex.substring (i, i + 4);
        return out;
    }

    inline juce::File licenseFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("PearlLeashPlugin").getChildFile ("Harmonic2K").getChildFile ("license.json");
    }

    inline bool checkLicense()
    {
        auto f = licenseFile();
        if (! f.existsAsFile()) return false;
        auto json = juce::JSON::parse (f.loadFileAsString());
        auto email  = json.getProperty ("email",  juce::var ("")).toString();
        auto serial = json.getProperty ("serial", juce::var ("")).toString().trim().toUpperCase();
        return email.isNotEmpty() && serial == expectedSerial (email);
    }

    inline bool activate (const juce::String& email, const juce::String& serial)
    {
        if (serial.trim().toUpperCase() != expectedSerial (email)) return false;
        auto f = licenseFile();
        f.getParentDirectory().createDirectory();
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("email", email.trim());
        obj->setProperty ("serial", serial.trim().toUpperCase());
        return f.replaceWithText (juce::JSON::toString (juce::var (obj)));
    }
}
