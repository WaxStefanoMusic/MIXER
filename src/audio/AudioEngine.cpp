#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace mixer::audio {

namespace {

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// Aggiorna atomic peak in modalit "max wins" (lock-free).
inline void updateMaxPeak(std::atomic<float>& a, float v) noexcept
{
    float prev = a.load(std::memory_order_relaxed);
    while (v > prev
           && !a.compare_exchange_weak(prev, v, std::memory_order_relaxed))
        ;
}

// Calcola peak L/R su un buffer interleaved con cap_ch canali.
// Se mono, R = L. Restituisce abs peak per canale.
inline void computePeakLR(const float* data, uint32_t frames, uint32_t cap_ch,
                          float gain, float& peakL, float& peakR) noexcept
{
    float pL = 0.0f, pR = 0.0f;
    if (cap_ch == 1)
    {
        for (uint32_t f = 0; f < frames; ++f)
        {
            const float v = std::fabs(data[f] * gain);
            if (v > pL) pL = v;
        }
        pR = pL;
    }
    else
    {
        for (uint32_t f = 0; f < frames; ++f)
        {
            const float l = std::fabs(data[f * cap_ch + 0] * gain);
            const float r = std::fabs(data[f * cap_ch + 1] * gain);
            if (l > pL) pL = l;
            if (r > pR) pR = r;
        }
    }
    peakL = pL;
    peakR = pR;
}

// Mix il blocco capture nel buffer di destinazione secondo il channel layout.
// Aggiunge (+=) i sample moltiplicati per `gain`.
void mixInto(float* dst, uint32_t dst_frames, uint32_t dst_ch,
             const float* src, uint32_t src_ch,
             float gain)
{
    if (dst_ch == src_ch)
    {
        const uint32_t total = dst_frames * dst_ch;
        for (uint32_t i = 0; i < total; ++i) dst[i] += src[i] * gain;
        return;
    }
    if (src_ch == 1 && dst_ch >= 2)
    {
        // Mono -> broadcast su FL+FR, gli altri canali silenti.
        for (uint32_t f = 0; f < dst_frames; ++f)
        {
            const float s = src[f] * gain;
            dst[f * dst_ch + 0] += s;
            dst[f * dst_ch + 1] += s;
        }
        return;
    }
    if (src_ch >= 2 && dst_ch == 1)
    {
        // Stereo+ -> mono: media dei primi due canali.
        for (uint32_t f = 0; f < dst_frames; ++f)
            dst[f] += (src[f * src_ch + 0] + src[f * src_ch + 1]) * 0.5f * gain;
        return;
    }
    if (src_ch == 2 && dst_ch >= 4)
    {
        // Stereo -> 5.1/7.1: solo FL/FR; FC/LFE/surround a 0.
        for (uint32_t f = 0; f < dst_frames; ++f)
        {
            dst[f * dst_ch + 0] += src[f * 2 + 0] * gain;
            dst[f * dst_ch + 1] += src[f * 2 + 1] * gain;
        }
        return;
    }
    if (src_ch >= 4 && dst_ch == 2)
    {
        // 5.1/7.1 -> stereo: downmix ITU-R BS.775 semplificato.
        // L = FL + 0.707*FC + 0.5*BL
        // R = FR + 0.707*FC + 0.5*BR
        // (se mancano canali, li trattiamo come 0)
        for (uint32_t f = 0; f < dst_frames; ++f)
        {
            const float fl = src[f * src_ch + 0];
            const float fr = src[f * src_ch + 1];
            const float fc = (src_ch > 2) ? src[f * src_ch + 2] : 0.0f;
            const float bl = (src_ch > 4) ? src[f * src_ch + 4] : 0.0f;
            const float br = (src_ch > 5) ? src[f * src_ch + 5] : 0.0f;
            dst[f * 2 + 0] += (fl + 0.707f * fc + 0.5f * bl) * gain;
            dst[f * 2 + 1] += (fr + 0.707f * fc + 0.5f * br) * gain;
        }
        return;
    }
    // Fallback: copia min(src_ch, dst_ch) canali, ignora gli altri.
    const uint32_t cm = (src_ch < dst_ch) ? src_ch : dst_ch;
    for (uint32_t f = 0; f < dst_frames; ++f)
        for (uint32_t c = 0; c < cm; ++c)
            dst[f * dst_ch + c] += src[f * src_ch + c] * gain;
}

// Chiave per identificare univocamente una cattura (device + modalit).
std::wstring captureKey(const std::wstring& device_id, bool loopback)
{
    return (loopback ? L"L|" : L"C|") + device_id;
}

} // namespace

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const config::MixerCfg& cfg, int buffer_ms,
                        bool exclusive, bool lowlat)
{
    if (running_.load()) return false;
    stopAllStreams();
    last_error_.store(0, std::memory_order_release);

    // 1) Costruisci snapshot dei descrittori di strip e bus (immutabili
    //    mentre l'engine gira).
    strips_.clear();
    strips_.reserve(cfg.strips.size());
    for (const auto& sc : cfg.strips)
    {
        auto s = std::make_unique<StripRT>();
        s->device_id = sc.device_id;
        s->loopback  = sc.loopback;
        s->gain_lin.store(dbToLin(sc.gain_db));
        s->mute.store(sc.mute);
        s->solo.store(sc.solo);
        s->routes.reserve(cfg.buses.size());
        s->route_level.reserve(cfg.buses.size());
        for (size_t b = 0; b < cfg.buses.size(); ++b)
        {
            auto a = std::make_unique<std::atomic<bool>>(false);
            if (b < sc.routes.size()) a->store(sc.routes[b]);
            s->routes.push_back(std::move(a));

            auto lvl = std::make_unique<std::atomic<float>>(1.0f);
            if (b < sc.route_level.size()) lvl->store(sc.route_level[b]);
            s->route_level.push_back(std::move(lvl));
        }
        strips_.push_back(std::move(s));
    }
    buses_.clear();
    buses_.reserve(cfg.buses.size());
    for (const auto& bc : cfg.buses)
    {
        auto b = std::make_unique<BusRT>();
        b->device_id = bc.device_id;
        b->gain_lin.store(dbToLin(bc.gain_db));
        b->mute.store(bc.mute);
        b->solo.store(bc.solo);
        buses_.push_back(std::move(b));
    }

    // Ricomputa any_solo (strip) e any_bus_solo (bus)
    bool any_solo = false;
    for (auto& s : strips_) if (s->solo.load()) { any_solo = true; break; }
    any_solo_.store(any_solo);
    bool any_bus_solo = false;
    for (auto& b : buses_) if (b->solo.load()) { any_bus_solo = true; break; }
    any_bus_solo_.store(any_bus_solo);

    // 2) Determina i bus "attivi" (con device assegnato).
    std::vector<bool> bus_active(buses_.size(), false);
    for (size_t b = 0; b < buses_.size(); ++b)
        bus_active[b] = !buses_[b]->device_id.empty();

    // Inizializza reader_idx_per_bus per tutte le strip (default -1 = non routata).
    for (auto& sp : strips_)
        sp->reader_idx_per_bus.assign(buses_.size(), -1);

    // PASS 1: conta i lettori (consumer) necessari per ciascuna chiave capture.
    // Una strip routata a N bus attivi richiede N reader nel SUO capture.
    // Capture condivise tra pi strip sommano i lettori.
    std::map<std::wstring, size_t> reader_demand;
    for (size_t i = 0; i < strips_.size(); ++i)
    {
        auto& s = *strips_[i];
        if (s.device_id.empty()) continue;
        const std::wstring key = captureKey(s.device_id, s.loopback);
        for (size_t b = 0; b < buses_.size(); ++b)
        {
            if (!bus_active[b]) continue;
            if (b >= s.routes.size() || !s.routes[b]->load()) continue;
            ++reader_demand[key];
        }
    }

    // PASS 2: apri una capture per chiave, con setReaderCount(N).
    captures_.clear();
    std::map<std::wstring, int> capture_idx_by_key;
    for (auto& kv : reader_demand)
    {
        const std::wstring& key = kv.first;
        const size_t count = kv.second;
        if (key.size() < 3) continue;
        const bool loopback = (key[0] == L'L');
        const std::wstring dev_id = key.substr(2);

        auto cap = std::make_unique<WasapiCaptureStream>();
        cap->setReaderCount(count);
        // Tenta exclusive/lowlat (internamente: loopback forza shared per
        // exclusive ma IAC3 lowlat funziona anche col loopback). Se fallisce,
        // ricrea e riprova in shared normale.
        bool ok = cap->start(dev_id, loopback, buffer_ms, 200,
                             exclusive, lowlat);
        if (!ok && (exclusive || lowlat))
        {
            cap = std::make_unique<WasapiCaptureStream>();
            cap->setReaderCount(count);
            ok = cap->start(dev_id, loopback, buffer_ms, 200, false, false);
        }
        if (!ok)
        {
            last_error_.store(cap->lastError(), std::memory_order_release);
            continue;
        }
        capture_idx_by_key[key] = (int)captures_.size();
        captures_.push_back(std::move(cap));
    }

    // PASS 3: assegna reader_idx_per_bus per ogni (strip, bus) routato.
    // I reader vengono allocati sequenzialmente per ogni chiave capture.
    std::map<std::wstring, size_t> next_reader_for_key;
    for (size_t i = 0; i < strips_.size(); ++i)
    {
        auto& s = *strips_[i];
        s.capture_idx = -1;
        if (s.device_id.empty()) continue;
        const std::wstring key = captureKey(s.device_id, s.loopback);
        auto it = capture_idx_by_key.find(key);
        if (it == capture_idx_by_key.end()) continue;  // capture failed to open
        s.capture_idx = it->second;
        for (size_t b = 0; b < buses_.size(); ++b)
        {
            if (!bus_active[b]) continue;
            if (b >= s.routes.size() || !s.routes[b]->load()) continue;
            s.reader_idx_per_bus[b] = (int)next_reader_for_key[key]++;
        }
    }

    // 3) Apri i render stream per ogni bus attivo che ha almeno una strip
    //    capture-aperta routata.
    renders_.clear();
    int started_renders = 0;
    for (size_t b = 0; b < buses_.size(); ++b)
    {
        if (!bus_active[b]) { buses_[b]->render_idx = -1; continue; }

        bool any_strip = false;
        for (size_t i = 0; i < strips_.size(); ++i)
        {
            auto& s = strips_[i];
            if (s->capture_idx < 0) continue;
            if (b < s->routes.size() && s->routes[b]->load())
            { any_strip = true; break; }
        }
        if (!any_strip) { buses_[b]->render_idx = -1; continue; }

        auto render = std::make_unique<WasapiRenderStream>();
        const int bus_index = (int)b;

        // Callback eseguito sul thread audio del render.
        // NIENTE alloc/lock/syscall qui dentro: solo memcpy/atomic/math.
        // Reuse di un thread_local buffer per la lettura dalla cattura.
        auto pull = [this, bus_index]
            (float* dst, uint32_t frames, uint32_t out_ch, uint32_t /*sr*/)
        {
            // Clear output.
            std::memset(dst, 0, (size_t)frames * out_ch * sizeof(float));

            const bool bypass   = bypass_gains_.load(std::memory_order_relaxed);
            const bool any_solo = any_solo_.load(std::memory_order_relaxed);
            BusRT& bus = *buses_[bus_index];
            const float bus_gain = bypass
                ? 1.0f
                : bus.gain_lin.load(std::memory_order_relaxed);

            if (bus.mute.load(std::memory_order_relaxed))
                return;  // dst gi a zero
            // Solo a livello bus: se un qualsiasi bus e' in solo, i bus NON in
            // solo restano muti.
            if (any_bus_solo_.load(std::memory_order_relaxed)
                && !bus.solo.load(std::memory_order_relaxed))
                return;

            // Buffer di lavoro thread_local per leggere da una capture.
            thread_local std::vector<float> tmp;

            for (size_t i = 0; i < strips_.size(); ++i)
            {
                StripRT& s = *strips_[i];
                if (s.capture_idx < 0) continue;
                if ((size_t)bus_index >= s.reader_idx_per_bus.size()) continue;
                const int reader_idx = s.reader_idx_per_bus[bus_index];
                if (reader_idx < 0) continue;     // non routata a questo bus
                // (la rotta runtime pu venire mutata via updateParams ma
                //  reader_idx_per_bus  fisso una volta avviato l'engine; per
                //  rispettare il toggle a runtime controlliamo anche s.routes)
                if (!s.routes[bus_index]->load(std::memory_order_relaxed)) continue;
                if (s.mute.load(std::memory_order_relaxed)) continue;
                if (any_solo && !s.solo.load(std::memory_order_relaxed)) continue;

                WasapiCaptureStream& cap = *captures_[s.capture_idx];
                const uint32_t cap_ch = cap.channels();
                if (cap_ch == 0) continue;

                const size_t want = (size_t)frames * cap_ch;
                if (tmp.size() < want) tmp.resize(want);
                const size_t got = cap.read((size_t)reader_idx, tmp.data(), want);
                if (got < want)
                    std::memset(tmp.data() + got, 0, (want - got) * sizeof(float));

                const float strip_gain = bypass
                    ? 1.0f
                    : s.gain_lin.load(std::memory_order_relaxed);

                // Peak meter di strip post-fader (gain di strip, NON di bus):
                // se la stessa strip va su pi bus, il peak  identico.
                float pL = 0.0f, pR = 0.0f;
                computePeakLR(tmp.data(), frames, cap_ch, strip_gain, pL, pR);
                updateMaxPeak(s.peak_meter[0], pL);
                updateMaxPeak(s.peak_meter[1], pR);

                // Livello di invio per-bus (0..1). Si applica SEMPRE, anche in
                // bypass: non  il fader dBFS ma un attenuatore di routing
                // scelto dall'utente (es. questa strip al 50% su Bus 1, 100%
                // su Bus 2). Il peak meter di strip resta indipendente.
                const float route_lvl =
                    ((size_t)bus_index < s.route_level.size())
                        ? s.route_level[bus_index]->load(std::memory_order_relaxed)
                        : 1.0f;

                mixInto(dst, frames, out_ch, tmp.data(), cap_ch,
                        strip_gain * bus_gain * route_lvl);
            }

            // Limite soft (clip a 1.0 lineare) attivo SOLO quando dBFS attivo.
            // In bypass passiamo l'audio cos com', niente clipping.
            if (!bypass)
            {
                const uint32_t total = frames * out_ch;
                for (uint32_t i = 0; i < total; ++i)
                {
                    if (dst[i] >  1.0f) dst[i] =  1.0f;
                    if (dst[i] < -1.0f) dst[i] = -1.0f;
                }
            }

            // Peak meter del bus  uscita finale (post-mix, post-clip).
            float bpL = 0.0f, bpR = 0.0f;
            if (out_ch == 1)
            {
                for (uint32_t f = 0; f < frames; ++f)
                {
                    const float v = std::fabs(dst[f]);
                    if (v > bpL) bpL = v;
                }
                bpR = bpL;
            }
            else
            {
                for (uint32_t f = 0; f < frames; ++f)
                {
                    const float l = std::fabs(dst[f * out_ch + 0]);
                    const float r = std::fabs(dst[f * out_ch + 1]);
                    if (l > bpL) bpL = l;
                    if (r > bpR) bpR = r;
                }
            }
            updateMaxPeak(bus.peak_meter[0], bpL);
            updateMaxPeak(bus.peak_meter[1], bpR);
        };

        // Tenta exclusive/lowlat; se fallisce, ricrea e riprova in shared
        // normale cos il bus parte comunque.
        bool rok = render->start(buses_[b]->device_id, pull, buffer_ms,
                                 exclusive, lowlat);
        if (!rok && (exclusive || lowlat))
        {
            render = std::make_unique<WasapiRenderStream>();
            rok = render->start(buses_[b]->device_id, pull, buffer_ms,
                                 false, false);
        }
        if (!rok)
        {
            last_error_.store(render->lastError(), std::memory_order_release);
            continue;
        }
        buses_[b]->render_idx = (int)renders_.size();
        renders_.push_back(std::move(render));
        ++started_renders;
    }

    if (started_renders == 0)
    {
        // Niente bus partito: ferma anche le capture.
        stopAllStreams();
        return false;
    }

    running_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::stop()
{
    stopAllStreams();
    running_.store(false, std::memory_order_release);
}

void AudioEngine::stopAllStreams()
{
    for (auto& r : renders_)  if (r) r->stop();
    for (auto& c : captures_) if (c) c->stop();
    renders_.clear();
    captures_.clear();
}

void AudioEngine::updateParams(const config::MixerCfg& cfg)
{
    // Aggiorna solo i parametri "live", non i device assignment (richiedono
    // restart per riaprire gli stream).
    const size_t ns = std::min(strips_.size(), cfg.strips.size());
    for (size_t i = 0; i < ns; ++i)
    {
        auto& s = *strips_[i];
        const auto& sc = cfg.strips[i];
        s.gain_lin.store(dbToLin(sc.gain_db), std::memory_order_relaxed);
        s.mute.store(sc.mute, std::memory_order_relaxed);
        s.solo.store(sc.solo, std::memory_order_relaxed);
        const size_t nb = std::min(s.routes.size(), sc.routes.size());
        for (size_t b = 0; b < nb; ++b)
            s.routes[b]->store(sc.routes[b], std::memory_order_relaxed);
        const size_t nl = std::min(s.route_level.size(), sc.route_level.size());
        for (size_t b = 0; b < nl; ++b)
            s.route_level[b]->store(sc.route_level[b], std::memory_order_relaxed);
    }
    const size_t nb = std::min(buses_.size(), cfg.buses.size());
    for (size_t i = 0; i < nb; ++i)
    {
        auto& b = *buses_[i];
        const auto& bc = cfg.buses[i];
        b.gain_lin.store(dbToLin(bc.gain_db), std::memory_order_relaxed);
        b.mute.store(bc.mute, std::memory_order_relaxed);
        b.solo.store(bc.solo, std::memory_order_relaxed);
    }

    bool any_solo = false;
    for (auto& s : strips_) if (s->solo.load()) { any_solo = true; break; }
    any_solo_.store(any_solo, std::memory_order_relaxed);
    bool any_bus_solo = false;
    for (auto& b : buses_) if (b->solo.load()) { any_bus_solo = true; break; }
    any_bus_solo_.store(any_bus_solo, std::memory_order_relaxed);
}

float AudioEngine::readStripPeak(size_t i, int ch)
{
    if (i >= strips_.size() || ch < 0 || ch >= 2) return 0.0f;
    return strips_[i]->peak_meter[ch].exchange(0.0f, std::memory_order_relaxed);
}

float AudioEngine::readBusPeak(size_t i, int ch)
{
    if (i >= buses_.size() || ch < 0 || ch >= 2) return 0.0f;
    return buses_[i]->peak_meter[ch].exchange(0.0f, std::memory_order_relaxed);
}

AudioEngine::Status AudioEngine::getStatus() const
{
    Status st;
    st.running = running_.load();
    st.captures_opened = captures_.size();
    st.renders_opened  = renders_.size();
    st.last_error = last_error_.load();
    std::ostringstream ss;
    ss << "captures=" << st.captures_opened
       << " renders=" << st.renders_opened;
    if (st.last_error != 0)
        ss << " err=0x" << std::hex << st.last_error;
    st.description = ss.str();
    return st;
}

} // namespace mixer::audio
