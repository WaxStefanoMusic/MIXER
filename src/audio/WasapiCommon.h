#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace mixer::audio {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Apartment-threaded COM init scoped al thread chiamante.
// Idempotente: pi istanze sullo stesso thread sono OK (CoInitializeEx ref-counta).
struct ScopedComInit
{
    HRESULT hr;
    ScopedComInit()  { hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ScopedComInit() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

// Apre IMMDevice da PnP id (wstring).
inline HRESULT openDeviceById(const std::wstring& id, ComPtr<IMMDevice>& out)
{
    ComPtr<IMMDeviceEnumerator> enumr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumr));
    if (FAILED(hr)) return hr;
    return enumr->GetDevice(id.c_str(), &out);
}

// Verifica che il mix format del device sia compatibile con il nostro engine
// (FLOAT32 interleaved). Ritorna nullptr se il formato non  utilizzabile cos
// com', il chiamante decider se convertire.
inline bool isFloat32Format(const WAVEFORMATEX* wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32)
        return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
               && wfx->wBitsPerSample == 32;
    }
    return false;
}

// ---- Formato campioni per Exclusive mode -----------------------------------
// In exclusive il device spesso NON accetta float32; bisogna negoziare un
// formato PCM intero e convertire al boundary WASAPI.
enum class SampleFmt { Float32, Int16, Int32 };

// Conversioni interleaved (n = numero totale di sample, non frame).
inline void f32_to_i16(const float* s, int16_t* d, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
    {
        float v = s[i] * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        d[i] = (int16_t)v;
    }
}
inline void i16_to_f32(const int16_t* s, float* d, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i) d[i] = (float)s[i] * (1.0f / 32768.0f);
}
inline void f32_to_i32(const float* s, int32_t* d, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
    {
        double v = (double)s[i] * 2147483647.0;
        if (v >  2147483647.0) v =  2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        d[i] = (int32_t)v;
    }
}
inline void i32_to_f32(const int32_t* s, float* d, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
        d[i] = (float)((double)s[i] * (1.0 / 2147483648.0));
}

// Costruisce un WAVEFORMATEXTENSIBLE per il formato/rate/canali dati.
inline WAVEFORMATEXTENSIBLE makeFormatExt(SampleFmt f, uint32_t rate,
                                          uint16_t ch)
{
    WAVEFORMATEXTENSIBLE w = {};
    w.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels       = ch;
    w.Format.nSamplesPerSec  = rate;
    w.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE)
                               - sizeof(WAVEFORMATEX);
    switch (f)
    {
    case SampleFmt::Float32:
        w.Format.wBitsPerSample = 32;
        w.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case SampleFmt::Int16:
        w.Format.wBitsPerSample = 16;
        w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case SampleFmt::Int32:
        w.Format.wBitsPerSample = 32;
        w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    }
    w.Format.nBlockAlign     = (WORD)(ch * w.Format.wBitsPerSample / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    w.Samples.wValidBitsPerSample = w.Format.wBitsPerSample;
    w.dwChannelMask = (ch == 2)
        ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
        : (ch == 1 ? SPEAKER_FRONT_CENTER : 0);
    return w;
}

// Negozia un formato supportato dal device in EXCLUSIVE mode.
// Prova: mix format float32 -> Int32 -> Int16, al sample rate/canali del mix.
// Ritorna true e riempie out_wfx + out_fmt se trova qualcosa; false altrimenti.
inline bool negotiateExclusive(IAudioClient* client,
                               const WAVEFORMATEX* mix,
                               WAVEFORMATEXTENSIBLE& out_wfx,
                               SampleFmt& out_fmt)
{
    if (!client || !mix) return false;
    const uint32_t rate = mix->nSamplesPerSec;
    const uint16_t ch   = mix->nChannels;

    const SampleFmt order[3] = { SampleFmt::Float32,
                                 SampleFmt::Int32,
                                 SampleFmt::Int16 };
    for (SampleFmt f : order)
    {
        WAVEFORMATEXTENSIBLE w = makeFormatExt(f, rate, ch);
        HRESULT hr = client->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            reinterpret_cast<const WAVEFORMATEX*>(&w), nullptr);
        if (hr == S_OK)
        {
            out_wfx = w;
            out_fmt = f;
            return true;
        }
    }
    return false;
}

// ---- IAudioClient3 low-latency shared --------------------------------------
// Sceglie un period (in frame) valido per InitializeSharedAudioStream:
// parte dal buffer richiesto in ms, lo clampa a [min,max] e lo arrotonda a
// un multiplo del fundamental period (vincolo dell'API).
inline UINT32 pickIac3Period(uint32_t sample_rate, int buffer_ms,
                             UINT32 minF, UINT32 maxF, UINT32 fundF)
{
    if (fundF == 0) fundF = 1;
    uint64_t want = (uint64_t)sample_rate * (uint64_t)buffer_ms / 1000ull;
    if (want < minF) want = minF;
    if (want > maxF) want = maxF;
    UINT32 r = (UINT32)(((want + fundF / 2) / fundF) * fundF);
    if (r < minF) r = minF;
    if (r > maxF) r = maxF;
    return r;
}

// Tenta di inizializzare `client` in IAudioClient3 low-latency shared con
// il formato `fmt`. streamFlags pu includere LOOPBACK/EVENTCALLBACK.
// Ritorna true se l'init IAC3  riuscito (period scelto in out_period_frames).
// Se ritorna false il client NON  inizializzato (il chiamante deve fare il
// fallback al normale IAudioClient::Initialize, eventualmente ri-attivando).
inline bool tryInitLowLatencyShared(IAudioClient* client,
                                    const WAVEFORMATEX* fmt,
                                    uint32_t sample_rate,
                                    int buffer_ms,
                                    DWORD streamFlags,
                                    UINT32& out_period_frames)
{
    if (!client || !fmt) return false;
    ComPtr<IAudioClient3> c3;
    if (FAILED(client->QueryInterface(__uuidof(IAudioClient3),
                                      (void**)c3.GetAddressOf())) || !c3)
        return false;

    UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
    if (FAILED(c3->GetSharedModeEnginePeriod(fmt, &defF, &fundF, &minF, &maxF)))
        return false;
    if (minF == 0 || maxF == 0) return false;

    const UINT32 period = pickIac3Period(sample_rate, buffer_ms,
                                         minF, maxF, fundF);
    HRESULT hr = c3->InitializeSharedAudioStream(streamFlags, period,
                                                 fmt, nullptr);
    if (FAILED(hr)) return false;
    out_period_frames = period;
    return true;
}

} // namespace mixer::audio
