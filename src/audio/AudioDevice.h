#pragma once

#include <string>
#include <cstdint>

namespace mixer::audio {

enum class DeviceFlow
{
    Render,   // playback / loopback capture
    Capture,  // microphones, line-in
};

// Metadati di un endpoint WASAPI gi a portata dell'app.
// Non possiede l'IMMDevice COM, solo info statiche.
struct AudioDevice
{
    std::wstring id;            // PnP id (unico, persistente)
    std::wstring friendly_name; // "Realtek USB Audio (USB Headset)"
    DeviceFlow   flow = DeviceFlow::Render;
    uint32_t     sample_rate = 0;   // mix format del device
    uint16_t     channels    = 0;   // mix format del device
    uint32_t     channel_mask = 0;  // SPEAKER_FRONT_LEFT | ... (Windows)
    bool         is_default = false;
};

} // namespace mixer::audio
