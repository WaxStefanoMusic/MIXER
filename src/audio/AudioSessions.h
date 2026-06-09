#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mixer::audio {

// Informazioni su una sessione audio (app che sta producendo audio sul sistema).
struct AppSession
{
    uint32_t      process_id    = 0;          // PID; 0 = system sounds
    std::wstring  process_name;                // "spotify.exe", "discord.exe"
    std::wstring  display_name;                // "Spotify", "Discord"  (pu coincidere)
    std::wstring  current_endpoint_id;         // device id su cui la sessione gira ora
    std::wstring  current_endpoint_name;       // friendly name di quel device
    // override_endpoint_id: device esplicitamente impostato dall'utente per
    // questa app via "Mixer volume" di Windows (o dal nostro pannello).
    // Stringa vuota = nessun override, app usa il predefinito di sistema.
    std::wstring  override_endpoint_id;
    float         peak    = 0.0f;              // peak corrente (0..1+)
    bool          is_playing = false;          // active = produce audio adesso
    bool          is_system_sounds = false;
};

// Enumera tutte le sessioni audio sui device render attivi.
// Le sessioni "duplicate" (stessa pid su pi device) vengono unite in una sola
// entry (utile per non vedere la stessa app 5 volte in lista).
std::vector<AppSession> enumerateAppSessions();

} // namespace mixer::audio
