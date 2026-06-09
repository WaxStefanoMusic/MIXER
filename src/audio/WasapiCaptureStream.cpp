#include "WasapiCaptureStream.h"
#include "WasapiCommon.h"

#include <algorithm>
#include <avrt.h>
#include <cstring>

namespace mixer::audio {

namespace {
constexpr REFERENCE_TIME REFTIME_PER_MS = 10000;
}

WasapiCaptureStream::WasapiCaptureStream() = default;

WasapiCaptureStream::~WasapiCaptureStream()
{
    stop();
}

bool WasapiCaptureStream::start(const std::wstring& device_id,
                                bool loopback,
                                int buffer_ms,
                                int ring_ms,
                                bool exclusive,
                                bool lowlat)
{
    if (running_.load()) return false;
    if (device_id.empty()) return false;

    device_id_  = device_id;
    loopback_   = loopback;
    buffer_ms_  = std::clamp(buffer_ms, 3, 200);
    ring_ms_    = std::clamp(ring_ms, 50, 2000);
    // WASAPI non supporta loopback in exclusive: se loopback forziamo shared.
    exclusive_  = exclusive && !loopback;
    // IAC3 low-latency  SHARED, quindi compatibile col loopback (qui sta
    // il guadagno per il preset bypass). Disattivato se exclusive ha vinto.
    lowlat_     = lowlat && !exclusive_;
    exclusive_active_ = false;
    lowlat_active_    = false;
    stop_requested_.store(false, std::memory_order_release);
    last_error_.store(0, std::memory_order_release);
    total_frames_.store(0, std::memory_order_relaxed);
    overrun_frames_.store(0, std::memory_order_relaxed);

    thread_ = std::thread(&WasapiCaptureStream::threadMain, this);

    // Attendi che il thread parta (running=true) o termini con errore.
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

void WasapiCaptureStream::stop()
{
    if (!thread_.joinable()) return;
    stop_requested_.store(true, std::memory_order_release);
    thread_.join();
    running_.store(false, std::memory_order_release);
}

void WasapiCaptureStream::threadMain()
{
    ScopedComInit com;
    if (!com.ok())
    {
        last_error_.store(com.hr, std::memory_order_release);
        return;
    }

    ComPtr<IMMDevice> dev;
    HRESULT hr = openDeviceById(device_id_, dev);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }

    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix)
    {
        last_error_.store(hr, std::memory_order_release);
        return;
    }

    SampleFmt           dev_fmt = SampleFmt::Float32;
    WAVEFORMATEXTENSIBLE excl_wfx = {};
    const WAVEFORMATEX* use_fmt = mix;

    if (exclusive_)
    {
        if (!negotiateExclusive(client.Get(), mix, excl_wfx, dev_fmt))
        {
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

    // Allocate one ring buffer per reader (fan-out). I ring contengono
    // SEMPRE float32 (i consumer leggono float32); se il device  PCM intero
    // convertiamo prima del push.
    const size_t ring_samples =
        (size_t)sample_rate_ * channels_ * (size_t)ring_ms_ / 1000;
    rings_.clear();
    rings_.reserve(reader_count_);
    for (size_t r = 0; r < reader_count_; ++r)
        rings_.push_back(std::make_unique<RingBuffer>(ring_samples));

    if (exclusive_)
    {
        REFERENCE_TIME def_per = 0, min_per = 0;
        client->GetDevicePeriod(&def_per, &min_per);
        REFERENCE_TIME want = (REFERENCE_TIME)buffer_ms_ * REFTIME_PER_MS;
        if (want < min_per) want = min_per;

        const DWORD eflags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, eflags,
                                want, want, use_fmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
        {
            UINT32 af = 0;
            client->GetBufferSize(&af);
            const REFERENCE_TIME aligned =
                (REFERENCE_TIME)((10000.0 * 1000.0
                    / use_fmt->nSamplesPerSec * af) + 0.5);
            client.Reset();
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, &client);
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, eflags,
                                        aligned, aligned, use_fmt, nullptr);
        }
        ::CoTaskMemFree(mix);
        if (FAILED(hr)) { last_error_.store(hr, std::memory_order_release); return; }
        exclusive_active_ = true;
    }
    else
    {
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        bool inited = false;

        // IAC3 low-latency shared (anche col loopback: resta shared).
        // Stesso formato float32 del mix  nessuna conversione.
        if (lowlat_)
        {
            UINT32 iac3_period = 0;
            if (tryInitLowLatencyShared(client.Get(), mix, sample_rate_,
                                        buffer_ms_, flags, iac3_period))
            {
                lowlat_active_ = true;
                inited = true;
            }
            else
            {
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

        if (!inited)
        {
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

    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);

    HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) { last_error_.store(E_FAIL, std::memory_order_release); return; }
    hr = client->SetEventHandle(evt);
    if (FAILED(hr))
    {
        last_error_.store(hr, std::memory_order_release);
        ::CloseHandle(evt);
        return;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr))
    {
        last_error_.store(hr, std::memory_order_release);
        ::CloseHandle(evt);
        return;
    }

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

    // Buffer locale per push silenti e per conversione PCM->float32.
    std::vector<float> silence_buf;
    std::vector<float> conv_buf;

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

        // Drain tutti i pacchetti pendenti.
        UINT32 packet_size = 0;
        if (FAILED(capture->GetNextPacketSize(&packet_size))) continue;
        while (packet_size > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD pkt_flags = 0;
            hr = capture->GetBuffer(&data, &frames, &pkt_flags, nullptr, nullptr);
            if (FAILED(hr) || frames == 0)
            {
                if (frames > 0) capture->ReleaseBuffer(frames);
                break;
            }

            const size_t samples_in_pkt = (size_t)frames * channels_;

            // Sorgente float32 da spingere nei ring:
            //  - silenzio  buffer di zeri
            //  - device float32  i sample del pacchetto direttamente
            //  - device PCM intero (exclusive)  converti in conv_buf
            const float* src;
            if (pkt_flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                if (silence_buf.size() < samples_in_pkt)
                    silence_buf.assign(samples_in_pkt, 0.0f);
                src = silence_buf.data();
            }
            else if (dev_fmt == SampleFmt::Float32)
            {
                src = reinterpret_cast<const float*>(data);
            }
            else
            {
                if (conv_buf.size() < samples_in_pkt)
                    conv_buf.resize(samples_in_pkt);
                if (dev_fmt == SampleFmt::Int16)
                    i16_to_f32(reinterpret_cast<const int16_t*>(data),
                               conv_buf.data(), samples_in_pkt);
                else /* Int32 */
                    i32_to_f32(reinterpret_cast<const int32_t*>(data),
                               conv_buf.data(), samples_in_pkt);
                src = conv_buf.data();
            }

            // Fan-out: scrivi gli stessi sample su tutti i ring (un consumer
            // per ring). Il ring "pi indietro" determina l'overrun count.
            size_t min_pushed = samples_in_pkt;
            for (auto& ring : rings_)
            {
                if (!ring) continue;
                const size_t pushed = ring->push(src, samples_in_pkt);
                if (pushed < min_pushed) min_pushed = pushed;
            }
            if (min_pushed < samples_in_pkt)
                overrun_frames_.fetch_add(
                    (samples_in_pkt - min_pushed) / channels_,
                    std::memory_order_relaxed);

            total_frames_.fetch_add(frames, std::memory_order_relaxed);
            capture->ReleaseBuffer(frames);

            if (FAILED(capture->GetNextPacketSize(&packet_size))) break;
        }
    }

    client->Stop();
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ::CloseHandle(evt);
    running_.store(false, std::memory_order_release);
}

} // namespace mixer::audio
