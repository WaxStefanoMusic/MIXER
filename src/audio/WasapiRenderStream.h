#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <cstdint>

namespace mixer::audio {

// Callback "pull": il thread render chiama questa funzione quando WASAPI
// chiede dati. Il callback DEVE riempire `frames * channels` float in `dst`.
// Eseguito sul thread audio: NIENTE malloc / lock / syscall bloccanti.
using RenderPullCallback = std::function<void(
    float*  dst,
    uint32_t frames,
    uint32_t channels,
    uint32_t sample_rate)>;

// Stream WASAPI in shared mode, event-driven, formato FLOAT32 interleaved.
// Il device viene aperto col suo mix format nativo per evitare conversioni
// lato Windows. Se il mix format non  float32, start() ritorna false.
class WasapiRenderStream
{
public:
    WasapiRenderStream();
    ~WasapiRenderStream();

    WasapiRenderStream(const WasapiRenderStream&) = delete;
    WasapiRenderStream& operator=(const WasapiRenderStream&) = delete;

    // device_id: PnP id (wstring) ottenuto da DeviceEnumerator.
    // pull: callback chiamato sul thread audio per riempire i buffer.
    // buffer_ms: durata richiesta del buffer device (clampata a [3..200]).
    // exclusive: se true tenta AUDCLNT_SHAREMODE_EXCLUSIVE (device monopolizzato).
    // lowlat:    se true (e !exclusive) tenta IAudioClient3 low-latency shared
    //            (periodo ~3 ms, resta condiviso). Fallback automatico a shared
    //            normale se il device non lo supporta.
    // Ritorna true se lo stream  partito.
    bool start(const std::wstring& device_id, RenderPullCallback pull,
               int buffer_ms = 30, bool exclusive = false,
               bool lowlat = false);
    void stop();

    bool isExclusive()   const noexcept { return exclusive_active_; }
    bool isLowLatency()  const noexcept { return lowlat_active_; }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // Info esposte dopo start() (zero prima).
    uint32_t sampleRate() const noexcept { return sample_rate_; }
    uint32_t channels()   const noexcept { return channels_; }
    uint32_t bufferFrames() const noexcept { return buffer_frames_; }

    // Ultimo errore HRESULT (per debug UI). 0 se nessun errore.
    long lastError() const noexcept { return last_error_.load(std::memory_order_acquire); }

private:
    void threadMain();

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    stop_requested_{false};
    std::atomic<long>    last_error_{0};

    std::wstring         device_id_;
    RenderPullCallback   pull_;
    int                  buffer_ms_ = 30;
    bool                 exclusive_ = false;        // richiesto
    bool                 exclusive_active_ = false; // effettivamente attivo
    bool                 lowlat_ = false;           // richiesto IAC3
    bool                 lowlat_active_ = false;    // IAC3 effettivamente attivo

    uint32_t             sample_rate_  = 0;
    uint32_t             channels_     = 0;
    uint32_t             buffer_frames_ = 0;
};

} // namespace mixer::audio
