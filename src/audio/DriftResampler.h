#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mixer::audio {

class WasapiCaptureStream;

// Compensazione del drift di clock tra un device di cattura e un device di
// render (es. VB-CABLE -> HDMI Elgato).
//
// Ogni device ha il proprio cristallo: "48 kHz" su due schede diverse non sono
// mai identici (differenza tipica 1..100 ppm). Senza compensazione il ring
// buffer tra cattura e render lentamente si svuota (-> buco di silenzio) o si
// riempie (-> sample scartati). Inoltre senza un livello di riempimento
// target basta un ritardo di scheduling di 1 ms per far mancare i dati.
//
// Questa classe, per UN lettore (coppia strip->bus):
//   - aspetta che nel ring ci sia un livello target prima di suonare (pre-roll)
//   - misura il livello medio del ring e corregge il rapporto di resampling
//     con un regolatore PI (correzione max +-0.2%, inudibile)
//   - fa il resampling con un filtro sinc finestrato polifase a 32 tap
//   - converte anche sample rate nominali diversi (es. 44.1k -> 48k)
//
// process() gira sul thread render: nessun lock, nessuna allocazione dopo
// prepare() (salvo richieste di frame eccezionalmente grandi).
class DriftResampler
{
public:
    // Da chiamare PRIMA di avviare il render (thread non real-time).
    void prepare(uint32_t in_rate, uint32_t channels);

    // Produce `out_frames` frame (canali = quelli della cattura) in `out`,
    // consumando dal lettore `reader` della cattura.
    void process(WasapiCaptureStream& cap, size_t reader,
                 float* out, uint32_t out_frames, uint32_t out_rate) noexcept;

    // Diagnostica (letta dalla UI).
    uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    uint32_t overflows() const noexcept { return overflows_.load(std::memory_order_relaxed); }
    // Correzione di drift attuale in ppm (solo diagnostica).
    float    driftPpm()  const noexcept { return drift_ppm_.load(std::memory_order_relaxed); }

private:
    void resetHistory() noexcept;

    uint32_t           in_rate_  = 0;
    uint32_t           channels_ = 0;

    std::vector<float> hist_;          // frame di ingresso interleaved
    size_t             hist_frames_ = 0;
    double             pos_ = 0.0;     // posizione frazionaria in hist_

    bool               primed_ = false;
    double             ema_level_ = 0.0;
    double             integ_ = 0.0;   // termine integrale (= drift stimato)
    uint32_t           max_req_ = 0;

    std::atomic<uint32_t> underruns_{0};
    std::atomic<uint32_t> overflows_{0};
    std::atomic<float>    drift_ppm_{0.0f};
};

} // namespace mixer::audio
