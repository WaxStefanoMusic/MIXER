#pragma once

#include "RingBuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace mixer::audio {

// Stream di cattura WASAPI shared mode, event-driven, formato FLOAT32 interleaved.
// Pu operare in due modalit:
//   - capture diretta:   device_id  un endpoint eCapture (microfono, line-in)
//   - loopback (rendere): device_id  un endpoint eRender; vengono catturati i
//     sample che vengono riprodotti su quel device (es. cosa esce da VB-CABLE A).
//
// L'audio catturato finisce in un RingBuffer SPSC interno. Il consumatore
// (tipicamente il thread render del bus) chiama read() per estrarne i sample.
class WasapiCaptureStream
{
public:
    WasapiCaptureStream();
    ~WasapiCaptureStream();

    WasapiCaptureStream(const WasapiCaptureStream&) = delete;
    WasapiCaptureStream& operator=(const WasapiCaptureStream&) = delete;

    // device_id: PnP id dell'endpoint (render o capture a seconda di loopback)
    // loopback : true = device_id deve essere eRender, catturiamo cio che vi esce
    // buffer_ms: durata richiesta del buffer device, default 30 ms
    // ring_ms  : durata del ring buffer SPSC interno, default 200 ms
    // exclusive: tenta AUDCLNT_SHAREMODE_EXCLUSIVE. NB: il loopback NON
    //            supportato in exclusive da WASAPI  se loopback==true la
    //            richiesta exclusive viene IGNORATA e si usa shared.
    bool start(const std::wstring& device_id,
               bool loopback,
               int buffer_ms = 30,
               int ring_ms   = 200,
               bool exclusive = false,
               bool lowlat   = false);

    bool isExclusive()  const noexcept { return exclusive_active_; }
    bool isLowLatency() const noexcept { return lowlat_active_; }

    // Numero di lettori indipendenti. DEVE essere chiamato PRIMA di start().
    // Default = 1. Ogni lettore ottiene il proprio ring buffer dedicato; il
    // thread di cattura scrive il dato in tutti i ring (fan-out). Necessario
    // quando una stessa strip  routata a pi bus  ogni bus  un consumer
    // distinto e non possono condividere un singolo ring SPSC.
    void setReaderCount(size_t n) { reader_count_ = (n == 0) ? 1 : n; }
    size_t readerCount() const noexcept { return reader_count_; }

    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    long lastError() const noexcept { return last_error_.load(std::memory_order_acquire); }

    uint32_t sampleRate()    const noexcept { return sample_rate_; }
    uint32_t channels()      const noexcept { return channels_; }
    bool     isLoopback()    const noexcept { return loopback_; }

    // Lettura dal ring buffer del lettore `reader_idx`. Ogni reader_idx ha
    // il suo ring buffer indipendente. Se reader_idx fuori range  0.
    size_t read(size_t reader_idx, float* dst, size_t samples) noexcept
    {
        if (reader_idx >= rings_.size() || !rings_[reader_idx]) return 0;
        return rings_[reader_idx]->pop(dst, samples);
    }

    // Quanti sample pronti nel ring buffer del lettore `reader_idx`.
    size_t available(size_t reader_idx) const noexcept
    {
        if (reader_idx >= rings_.size() || !rings_[reader_idx]) return 0;
        return rings_[reader_idx]->readAvailable();
    }

    // Statistiche per UI/diagnostica.
    uint64_t totalFramesCaptured() const noexcept
    {
        return total_frames_.load(std::memory_order_relaxed);
    }
    uint64_t overrunFrames() const noexcept
    {
        return overrun_frames_.load(std::memory_order_relaxed);
    }

private:
    void threadMain();

    std::thread                 thread_;
    std::atomic<bool>           running_{false};
    std::atomic<bool>           stop_requested_{false};
    std::atomic<long>           last_error_{0};
    std::atomic<uint64_t>       total_frames_{0};
    std::atomic<uint64_t>       overrun_frames_{0};

    std::wstring                device_id_;
    bool                        loopback_ = false;
    int                         buffer_ms_ = 30;
    int                         ring_ms_   = 200;
    size_t                      reader_count_ = 1;
    bool                        exclusive_ = false;
    bool                        exclusive_active_ = false;
    bool                        lowlat_ = false;
    bool                        lowlat_active_ = false;

    uint32_t                    sample_rate_ = 0;
    uint32_t                    channels_    = 0;

    // Un ring buffer per ciascun lettore (fan-out). Il thread di cattura
    // scrive in TUTTI; ciascun consumer pop dal proprio.
    std::vector<std::unique_ptr<RingBuffer>> rings_;
};

} // namespace mixer::audio
