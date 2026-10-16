#include "DriftResampler.h"
#include "WasapiCaptureStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mixer::audio {

namespace {

// ---- Filtro di interpolazione ---------------------------------------------
constexpr int    kTaps   = 64;          // lunghezza del filtro (pari)
constexpr int    kHalf   = kTaps / 2;
constexpr int    kPhases = 256;         // fasi tabulate (interp. lineare tra fasi)
constexpr double kCutoff = 0.94;        // frazione della Nyquist d'ingresso
constexpr double kBeta   = 9.0;         // Kaiser: ~ -90 dB di stopband

// ---- Regolatore di livello -------------------------------------------------
constexpr double kMarginMs  = 5.0;      // margine di sicurezza oltre a pacchetti/richieste
constexpr double kEmaTauS   = 0.5;      // media del livello del ring (s)
constexpr double kPTimeS    = 4.0;      // costante di tempo del termine P (s)
constexpr double kITimeS    = 16.0;     // costante di tempo del termine I (s)
constexpr double kMaxInteg  = 1000e-6;  // drift massimo stimabile (+-1000 ppm)
constexpr double kMaxCorr   = 2000e-6;  // correzione totale massima (+-0.2%)

struct Kernel
{
    float c[kPhases + 1][kTaps];
};

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double hx = x * 0.5;
    for (int k = 1; k < 64; ++k)
    {
        const double r = hx / k;
        term *= r * r;
        sum += term;
        if (term < 1e-14 * sum) break;
    }
    return sum;
}

// Tabella sinc finestrata (Kaiser). Riga p = ritardo frazionario p/kPhases.
// Il tap j pesa il campione di ingresso (i0 - (kHalf-1) + j).
const Kernel& kernel()
{
    static const Kernel* const k = [] {
        auto* kk = new Kernel;
        const double pi = 3.14159265358979323846;
        const double i0b = besselI0(kBeta);
        for (int p = 0; p <= kPhases; ++p)
        {
            const double frac = (double)p / kPhases;
            double sum = 0.0;
            double tmp[kTaps];
            for (int j = 0; j < kTaps; ++j)
            {
                const double t = (double)(j - (kHalf - 1)) - frac;
                const double x = kCutoff * t;
                const double s = (x == 0.0) ? 1.0 : std::sin(pi * x) / (pi * x);
                const double r = t / kHalf;
                const double w = (std::fabs(r) < 1.0)
                    ? besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0b
                    : 0.0;
                tmp[j] = kCutoff * s * w;
                sum += tmp[j];
            }
            // Guadagno DC unitario per ogni fase.
            for (int j = 0; j < kTaps; ++j)
                kk->c[p][j] = (float)(tmp[j] / sum);
        }
        return kk;
    }();
    return *k;
}

} // namespace

void DriftResampler::prepare(uint32_t in_rate, uint32_t channels)
{
    (void)kernel();  // costruisci la tabella fuori dal thread audio
    in_rate_  = in_rate;
    channels_ = channels;
    hist_.assign(((size_t)in_rate / 4 + 4 * kTaps) * (channels ? channels : 1),
                 0.0f);
    primed_    = false;
    ema_level_ = 0.0;
    integ_     = 0.0;
    max_req_   = 0;
    underruns_.store(0, std::memory_order_relaxed);
    overflows_.store(0, std::memory_order_relaxed);
    drift_ppm_.store(0.0f, std::memory_order_relaxed);
    resetHistory();
}

void DriftResampler::resetHistory() noexcept
{
    // kHalf-1 frame di "passato" a zero: il primo output e' centrato su
    // pos_ = kHalf-1 e il filtro ha gia' la sua storia.
    hist_frames_ = kHalf - 1;
    std::memset(hist_.data(), 0, hist_frames_ * channels_ * sizeof(float));
    pos_ = (double)(kHalf - 1);
}

void DriftResampler::process(WasapiCaptureStream& cap, size_t reader,
                             float* out, uint32_t out_frames,
                             uint32_t out_rate) noexcept
{
    const uint32_t ch = channels_;
    if (ch == 0 || in_rate_ == 0 || out_rate == 0 || out_frames == 0
        || cap.channels() != ch)
    {
        if (ch) std::memset(out, 0, (size_t)out_frames * ch * sizeof(float));
        return;
    }

    const double base_ratio = (double)in_rate_ / (double)out_rate;
    if (out_frames > max_req_) max_req_ = out_frames;

    // Livello target (frame di ingresso): una richiesta render + un
    // pacchetto di cattura INTERO + margine + la coda del filtro.
    // Il pacchetto intero serve perche' il livello misurato prima di ogni
    // richiesta dipende dalla fase tra eventi di cattura e di render: quando
    // (per il drift) la fase attraversa lo zero, il livello cala di colpo di
    // un pacchetto e il margine deve restare intatto anche in quel momento.
    uint32_t pkt = cap.maxPacketFrames();
    if (pkt == 0) pkt = in_rate_ / 100;
    const double target = (double)max_req_ * base_ratio
                        + pkt
                        + in_rate_ * kMarginMs / 1000.0
                        + kHalf;

    size_t ring_frames = cap.available(reader) / ch;
    double level = (double)ring_frames + ((double)hist_frames_ - pos_);

    if (!primed_)
    {
        // Pre-roll: silenzio finche' il ring non ha raggiunto il target.
        if (level < target)
        {
            std::memset(out, 0, (size_t)out_frames * ch * sizeof(float));
            return;
        }
        primed_    = true;
        ema_level_ = level;
    }
    else
    {
        // Accumulo anomalo (es. thread render rimasto fermo): scarta
        // l'eccesso invece di tenersi per sempre la latenza in piu'.
        const double limit = target + std::max(target, in_rate_ * 0.1);
        if (level > limit)
        {
            size_t excess = (size_t)(level - target);
            if (excess > ring_frames) excess = ring_frames;
            cap.skip(reader, excess * ch);
            ring_frames -= excess;
            level       -= (double)excess;
            ema_level_   = level;
            overflows_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Regolatore PI sul livello medio del ring.
    const double dt    = (double)out_frames / out_rate;
    const double alpha = std::min(1.0, dt / kEmaTauS);
    ema_level_ += alpha * (level - ema_level_);
    const double err = ema_level_ - target;     // >0: troppi dati -> consuma piu' in fretta
    const double kp  = 1.0 / (kPTimeS * in_rate_);
    integ_ += (kp / kITimeS) * err * dt;
    integ_  = std::clamp(integ_, -kMaxInteg, kMaxInteg);
    const double corr  = std::clamp(kp * err + integ_, -kMaxCorr, kMaxCorr);
    const double ratio = base_ratio * (1.0 + corr);
    drift_ppm_.store((float)(integ_ * 1e6), std::memory_order_relaxed);

    // Assicura in hist_ tutti i frame di ingresso che servono a questo blocco.
    const double last_pos = pos_ + (double)(out_frames - 1) * ratio;
    const size_t need = (size_t)last_pos + kHalf + 1;
    bool underrun = false;
    if (need > hist_frames_)
    {
        if (hist_.size() < need * ch) hist_.resize(need * ch);  // raro
        const size_t want = need - hist_frames_;
        const size_t got =
            cap.read(reader, &hist_[hist_frames_ * ch], want * ch) / ch;
        hist_frames_ += got;
        if (got < want)
        {
            std::memset(&hist_[hist_frames_ * ch], 0,
                        (want - got) * ch * sizeof(float));
            hist_frames_ = need;
            underrun = true;
        }
    }

    // Interpolazione sinc polifase.
    const Kernel& K = kernel();
    double pos = pos_;
    float coef[kTaps];
    for (uint32_t f = 0; f < out_frames; ++f)
    {
        const size_t i0   = (size_t)pos;
        const double ph   = (pos - (double)i0) * kPhases;
        size_t p0 = (size_t)ph;
        if (p0 >= (size_t)kPhases) p0 = kPhases - 1;
        const float a = (float)(ph - (double)p0);
        const float* c0 = K.c[p0];
        const float* c1 = K.c[p0 + 1];
        for (int j = 0; j < kTaps; ++j)
            coef[j] = c0[j] + a * (c1[j] - c0[j]);

        const float* src = &hist_[(i0 - (kHalf - 1)) * ch];
        float* dst = out + (size_t)f * ch;
        for (uint32_t c = 0; c < ch; ++c)
        {
            float acc = 0.0f;
            for (int j = 0; j < kTaps; ++j)
                acc += src[(size_t)j * ch + c] * coef[j];
            dst[c] = acc;
        }
        pos += ratio;
    }

    // Scarta i frame non piu' necessari, tenendo kHalf-1 frame di storia.
    size_t drop = (size_t)pos - (kHalf - 1);
    if (drop > hist_frames_) drop = hist_frames_;
    if (drop > 0)
    {
        std::memmove(hist_.data(), hist_.data() + drop * ch,
                     (hist_frames_ - drop) * ch * sizeof(float));
        hist_frames_ -= drop;
    }
    pos_ = pos - (double)drop;

    if (underrun)
    {
        // Conta solo i buchi "udibili": se la sorgente si e' semplicemente
        // fermata (loopback senza nulla in riproduzione) non e' un glitch.
        float pk = 0.0f;
        const size_t n = (size_t)out_frames * ch;
        for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::fabs(out[i]));
        if (pk > 1e-5f) underruns_.fetch_add(1, std::memory_order_relaxed);
        primed_ = false;
        resetHistory();
    }
}

} // namespace mixer::audio
