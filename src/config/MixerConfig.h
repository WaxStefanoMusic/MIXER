#pragma once

#include <string>
#include <vector>

namespace mixer::config {

// Configurazione di una singola strip d'ingresso.
// device_id pu essere vuoto: significa "non assegnato".
// loopback=true significa che device_id  un endpoint render aperto in
// modalit AUDCLNT_STREAMFLAGS_LOOPBACK (es. per catturare ci che esce
// da un VB-CABLE).
struct StripCfg
{
    std::string  label;
    std::wstring device_id;
    bool         loopback = false;
    float        gain_db  = 0.0f;
    bool         mute     = false;
    bool         solo     = false;
    // routes[b] = true se questa strip  inviata al bus b.
    // Lunghezza = numero di bus nella config.
    std::vector<bool> routes;
    // route_level[b] = volume di invio verso il bus b, 0.0..1.0 (1.0 = 100%).
    // Applicato SOLO se routes[b] == true. Permette p.es. di mandare la stessa
    // strip al Bus 1 al 50% e al Bus 2 al 100%. Default 1.0 (retro-compatibile
    // con preset che non hanno questa chiave). Lunghezza = numero di bus.
    std::vector<float> route_level;
};

struct BusCfg
{
    std::string  label;
    std::wstring device_id;  // sempre device render
    float        gain_db = 0.0f;
    bool         mute    = false;
};

struct MixerCfg
{
    int                    version = 1;
    std::vector<StripCfg>  strips;
    std::vector<BusCfg>    buses;

    // Allinea la lunghezza di ogni strip.routes a buses.size().
    void normalize();
};

// Config iniziale: 4 strip + 2 bus, niente di assegnato, niente route attive.
MixerCfg makeDefaultConfig();

// Salva su file (sovrascrive). Ritorna true se OK.
// Formato: testuale key=value con prefissi, vedi .cpp per i dettagli.
bool saveToFile(const std::wstring& path, const MixerCfg& cfg);

// Carica da file. In caso di errore, cfg non viene modificato.
bool loadFromFile(const std::wstring& path, MixerCfg& cfg);

} // namespace mixer::config
