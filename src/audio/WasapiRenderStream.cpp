#include "WasapiRenderStream.h"
#include "WasapiCommon.h"

#include <algorithm>
#include <avrt.h>
#include <cstring>
#include <vector>

namespace mixer::audio {

namespace {
constexpr REFERENCE_TIME REFTIME_PER_MS = 10000;
} // namespace

WasapiRenderStream::WasapiRenderStream() = default;

WasapiRenderStream::~WasapiRenderStream()
{
    stop();
}

bool WasapiRenderStream::start(const std::wstring& device_id, RenderPullCallback pull,
                                int buffer_ms, bool exclusive, bool lowlat)
{
    if (running_.load()) return false;
    if (!pull) return false;

    device_id_  = device_id;
    pull_       = std::move(pull);
    buffer_ms_  = std::clamp(buffer_ms, 3, 200);
    exclusive_  = exclusive;
    lowlat_     = lowlat && !exclusive;   // exclusive ha precedenza
    exclusive_active_ = false;
    lowlat_active_    = false;
    stop_requested_.store(false, std::memory_order_release);
    last_error_.store(0, std::memory_order_release);

    // Avvio thread; il thread far l'attivazione COM e l'apertura del client,
    // poi imposter running_=true. Aspettiamo qui che running_ diventi true
    // (o che il thread abbia gi fallito e sia terminato).
    thread_ = std::thread(&WasapiRenderStream::threadMain, this);

    // Wait fino a che running diventa true o thread esce.
    while (!running_.load(std::memory_order_acquire))
    {
        if (last_error_.load(std::memory_order_acquire) != 0)
        {
            if (thread_.joinable()) thread_.join();
            return false;
        }
        ::Sleep(1);
    }
    return true;
}

void WasapiRenderStream::stop()
{
    if (!thread_.joinable()) return;
    stop_requested_.store(true, std::memory_order_release);
    thread_.join();
    running_.store(false, std::memory_order_release);
}

void WasapiRenderStream::threadMain()
{
    ScopedComInit com;
    if (!com.ok())
    {
        last_error_.store(com.hr, std::memory_order_release);
        return;
    }

    // 1. Apri device.
    ComPtr<IMMDevice> dev;
    HRESULT hr = openDeviceById(device_id_, dev);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }

    // 2. Attiva IAudioClient.
    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }

    // 3. Mix format del device.
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { last_error_.store(hr, std::memory_order_release); return; }

    // Formato effettivo usato + se serve conversione float32->device.
    SampleFmt           dev_fmt = SampleFmt::Float32;
    WAVEFORMATEXTENSIBLE excl_wfx = {};
    const WAVEFORMATEX* use_fmt = mix;

    if (exclusive_)
    {
        if (!negotiateExclusive(client.Get(), mix, excl_wfx, dev_fmt))
        {
            // Nessun formato exclusive supportato: fallback gestito dal
            // chiamante (AudioEngine ritenter in shared).
            last_error_.store(AUDCLNT_E_UNSUPPORTED_FORMAT,
                              std::memory_order_release);
            ::CoTaskMemFree(mix);
            return;
        }
        use_fmt = reinterpret_cast<const WAVEFORMATEX*>(&excl_wfx);
    }
    else
    {
        if (!isFloat32Format(mix))
        {
            last_error_.store(AUDCLNT_E_UNSUPPORTED_FORMAT,
                              std::memory_order_release);
            ::CoTaskMemFree(mix);
            return;
        }
    }

    sample_rate_ = use_fmt->nSamplesPerSec;
    channels_    = use_fmt->nChannels;

    UINT32 frames_total = 0;

    if (exclusive_)
    {
        // EXCLUSIVE: event-driven, periodo = minimo del device per la
        // latenza pi bassa. Gestione AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED.
        REFERENCE_TIME def_per = 0, min_per = 0;
        client->GetDevicePeriod(&def_per, &min_per);
        REFERENCE_TIME want = (REFERENCE_TIME)buffer_ms_ * REFTIME_PER_MS;
        if (want < min_per) want = min_per;

        const DWORD eflags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, eflags,
                                want, want, use_fmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
        {
            UINT32 aligned_frames = 0;
            client->GetBufferSize(&aligned_frames);
            const REFERENCE_TIME aligned =
                (REFERENCE_TIME)((10000.0 * 1000.0
                    / use_fmt->nSamplesPerSec * aligned_frames) + 0.5);
            // Bisogna ri-attivare il client dopo questo errore.
            client.Reset();
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, &client);
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, eflags,
                                        aligned, aligned, use_fmt, nullptr);
        }
        ::CoTaskMemFree(mix);
        if (FAILED(hr))
        {
            last_error_.store(hr, std::memory_order_release);
            return;
        }
        exclusive_active_ = true;
    }
    else
    {
        bool inited = false;

        // 4a. IAudioClient3 low-latency shared (se richiesto). Stesso formato
        //     float32 del mix  nessuna conversione. Periodo ~3 ms se il
        //     driver lo supporta.
        if (lowlat_)
        {
            UINT32 iac3_period = 0;
            if (tryInitLowLatencyShared(client.Get(), mix, sample_rate_,
                                        buffer_ms_,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        iac3_period))
            {
                lowlat_active_ = true;
                inited = true;
            }
            else
            {
                // Init IAC3 fallita: ri-attiva il client pulito prima del
                // fallback al normale Initialize.
                client.Reset();
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                   nullptr, &client);
                if (FAILED(hr))
                {
                    last_error_.store(hr, std::memory_order_release);
                    ::CoTaskMemFree(mix);
                    return;
                }
            }
        }

        // 4b. SHARED normale: event-driven, autoconvert.
        if (!inited)
        {
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            const REFERENCE_TIME buffer_duration =
                (REFERENCE_TIME)buffer_ms_ * REFTIME_PER_MS;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    buffer_duration, 0, mix, nullptr);
            if (FAILED(hr))
            {
                ::CoTaskMemFree(mix);
                last_error_.store(hr, std::memory_order_release);
                return;
            }
        }
        ::CoTaskMemFree(mix);
    }

    hr = client->GetBufferSize(&frames_total);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }
    buffer_frames_ = frames_total;

    HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) { last_error_.store(E_FAIL, std::memory_order_release); return; }
    hr = client->SetEventHandle(evt);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); ::CloseHandle(evt); return; }

    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); ::CloseHandle(evt); return; }

    // 5. Pre-fill primo buffer (silenzio) per evitare glitch al primo start.
    //    La dimensione in byte dipende dal formato campione effettivo.
    const size_t bytes_per_sample =
        (dev_fmt == SampleFmt::Int16) ? 2u : 4u;  // Float32/Int32 = 4, Int16 = 2
    BYTE* pdata = nullptr;
    hr = render->GetBuffer(frames_total, &pdata);
    if (SUCCEEDED(hr) && pdata)
    {
        std::memset(pdata, 0,
                    (size_t)frames_total * channels_ * bytes_per_sample);
        render->ReleaseBuffer(frames_total, 0);
    }

    // 6. Aumenta priorit del thread audio via MMCSS.
    DWORD mmcss_task = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);

    hr = client->Start();
    if (FAILED(hr))
    {
        last_error_.store(hr, std::memory_order_release);
        if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
        ::CloseHandle(evt);
        return;
    }

    running_.store(true, std::memory_order_release);

    // Staging float32 per conversione (usato solo se dev_fmt != Float32).
    std::vector<float> stage;

    // In exclusive event-driven il padding non si usa: ogni evento  un
    // intero buffer da riempire. In shared usiamo frames_total - padding.
    const bool excl = exclusive_active_;

    // 7. Loop principale: aspetta evento, riempi buffer.
    while (!stop_requested_.load(std::memory_order_acquire))
    {
        DWORD wait = ::WaitForSingleObject(evt, 200);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0)
        {
            last_error_.store(HRESULT_FROM_WIN32(::GetLastError()),
                              std::memory_order_release);
            break;
        }

        UINT32 frames_avail;
        if (excl)
        {
            frames_avail = frames_total;
        }
        else
        {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            frames_avail = frames_total - padding;
        }
        if (frames_avail == 0) continue;

        BYTE* data = nullptr;
        hr = render->GetBuffer(frames_avail, &data);
        if (FAILED(hr) || !data)
        {
            last_error_.store(hr, std::memory_order_release);
            continue;
        }

        const size_t n = (size_t)frames_avail * channels_;
        if (dev_fmt == SampleFmt::Float32)
        {
            // Nessuna conversione: pull scrive direttamente nel buffer WASAPI.
            pull_(reinterpret_cast<float*>(data), frames_avail,
                  channels_, sample_rate_);
        }
        else
        {
            if (stage.size() < n) stage.resize(n);
            pull_(stage.data(), frames_avail, channels_, sample_rate_);
            if (dev_fmt == SampleFmt::Int16)
                f32_to_i16(stage.data(), reinterpret_cast<int16_t*>(data), n);
            else /* Int32 */
                f32_to_i32(stage.data(), reinterpret_cast<int32_t*>(data), n);
        }
        render->ReleaseBuffer(frames_avail, 0);
    }

    client->Stop();
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ::CloseHandle(evt);
    running_.store(false, std::memory_order_release);
}

} // namespace mixer::audio
