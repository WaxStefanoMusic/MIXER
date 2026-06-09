#pragma once

#include "WasapiCaptureStream.h"
#include "WasapiRenderStream.h"
#include "../config/MixerConfig.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mixer::audio {

// AudioEngine: collega i capture stream (dai device sorgente) ai render stream
// (dei bus) secondo la routing matrix definita in MixerCfg.
//
// Architettura:
//   - Per ogni bus con device assegnato e almeno una strip routata: 1 render
//     stream. Il pull callback del render somma i sample delle strip routate.
//   - Per ogni coppia unica (device_id, loopback) usata come sorgente: 1 capture
//     stream condiviso. Se due strip leggono dallo stesso device con stessa
//     modalit loopback, condividono la stessa cattura.
//   - I parametri runtime (gain, mute, solo, route attivo) sono std::atomic
//     aggiornati dal thread UI via updateParams(); il thread render li legge
//     senza lock.
//
// Limitazioni MVP:
//   - Sample rate dei device assunto uniforme (no resampling): se differiscono
//     l'audio suoner stonato. Tutti i device dell'utente sono 48 kHz.
//   - Channel layout: 1ch->2ch broadcast, 2ch->1ch average, altrimenti
//     copia min(in_ch, out_ch) canali.
//   - Niente parameter smoothing: muovere fader velocemente pu causare zipper.
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Apre i flussi necessari secondo cfg e li mette in run.
    // Ritorna true se almeno un render bus  partito.
    bool start(const config::MixerCfg& cfg, int buffer_ms = 30,
               bool exclusive = false, bool lowlat = false);
    void stop();
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // Aggiornamento parametri runtime senza riavviare gli stream.
    // Per cambi di device assegnato bisogna stop() + start().
    void updateParams(const config::MixerCfg& cfg);

    // Se true (default), i fader dB e il soft-clip sono BYPASSATI: l'audio
    // passa attraverso senza modifiche di guadagno. Utile per garantire che
    // il mixer non alteri il segnale finch l'utente non vuole esplicitamente
    // usare i controlli dBFS. Mute/solo restano comunque attivi.
    void setBypassGains(bool bypass) noexcept
    {
        bypass_gains_.store(bypass, std::memory_order_relaxed);
    }
    bool bypassGains() const noexcept
    {
        return bypass_gains_.load(std::memory_order_relaxed);
    }

    // Diagnostica per UI.
    struct Status
    {
        bool   running = false;
        size_t captures_opened = 0;
        size_t renders_opened  = 0;
        long   last_error = 0;
        std::string description;
    };
    Status getStatus() const;

    // Meter levels (post-fader): peak picco lineare 0..1+ (1.0 = 0 dBFS).
    // Le funzioni read*Peak() restituiscono il picco accumulato dall'ultima
    // chiamata e resettano l'accumulatore a 0  pattern "read & reset"
    // chiamabile dal thread UI senza lock.
    // ch  l'indice canale (0 = L, 1 = R). Indici fuori range  0.
    float readStripPeak(size_t strip_idx, int ch);
    float readBusPeak  (size_t bus_idx,   int ch);

    size_t stripCount() const { return strips_.size(); }
    size_t busCount()   const { return buses_.size(); }

private:
    struct StripRT
    {
        std::wstring        device_id;     // immutabile mentre engine gira
        bool                loopback = false;
        std::atomic<float>  gain_lin{1.0f};
        std::atomic<bool>   mute{false};
        std::atomic<bool>   solo{false};
        // routes[b].store(true) attiva la rotta verso bus b
        std::vector<std::unique_ptr<std::atomic<bool>>> routes;
        // route_level[b] = volume di invio verso il bus b (0..1, 1=100%).
        // Parallelo a routes; moltiplicato nel mix insieme a gain di strip/bus.
        // Si applica anche in bypass (NON  il fader dBFS:  un attenuatore
        // di routing impostato esplicitamente dall'utente).
        std::vector<std::unique_ptr<std::atomic<float>>> route_level;
        // Indice nel vettore captures_ a cui questa strip  attaccata (-1 se nessuna).
        int                 capture_idx = -1;
        // Per ogni bus, l'indice del lettore (reader) nel capture stream.
        // -1 = strip non routata a quel bus (o bus inattivo).
        // Necessario per il fan-out: ogni (strip,bus) consumer ha il suo ring.
        std::vector<int>    reader_idx_per_bus;
        // Peak meter post-fader (canali 0=L, 1=R). Aggiornato dal thread audio,
        // letto e azzerato dalla UI via readStripPeak().
        std::atomic<float>  peak_meter[2]{};
    };
    struct BusRT
    {
        std::wstring        device_id;
        std::atomic<float>  gain_lin{1.0f};
        std::atomic<bool>   mute{false};
        // Indice nel vettore renders_ (-1 se non aperto).
        int                 render_idx = -1;
        // Peak meter del bus (uscita post-mixing).
        std::atomic<float>  peak_meter[2]{};
    };

    void stopAllStreams();

    // Stato condiviso UI<->audio (atomic) e descrittore stream (mutato solo da UI).
    std::vector<std::unique_ptr<StripRT>>  strips_;
    std::vector<std::unique_ptr<BusRT>>    buses_;
    std::vector<std::unique_ptr<WasapiCaptureStream>> captures_;
    std::vector<std::unique_ptr<WasapiRenderStream>>  renders_;
    // any_solo_ ricalcolato in updateParams: serve nel render callback per non
    // dover scorrere tutte le strip ogni callback.
    std::atomic<bool>   any_solo_{false};
    std::atomic<bool>   running_{false};
    std::atomic<long>   last_error_{0};
    // True = bypass dBFS gain & soft-clip (passthrough puro).
    // Default false: i fader dB sono attivi. La UI (UiState.enable_dbfs)
    // pu togliere la spunta per attivare il bypass.
    std::atomic<bool>   bypass_gains_{false};
};

} // namespace mixer::audio
