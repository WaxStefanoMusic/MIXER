#pragma once

#include "AudioDevice.h"
#include <vector>

namespace mixer::audio {

// Enumera tutti gli endpoint WASAPI attivi (ACTIVE state) per la direzione data.
// Marca il default endpoint (eConsole role) con is_default=true.
// Non lancia: in caso di errore COM ritorna lista vuota.
//
// Thread-safety: la prima chiamata inizializza COM (apartment-threaded) sul thread
// chiamante. Chiamare dal thread UI, una volta, e cacheare il risultato.
std::vector<AudioDevice> enumerateDevices(DeviceFlow flow);

// Helper: enumera entrambe le direzioni.
struct DeviceList
{
    std::vector<AudioDevice> render;
    std::vector<AudioDevice> capture;
};
DeviceList enumerateAllDevices();

} // namespace mixer::audio
