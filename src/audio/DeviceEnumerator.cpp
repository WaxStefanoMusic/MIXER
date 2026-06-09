#include "DeviceEnumerator.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <propvarutil.h>

#include <wrl/client.h>

namespace mixer::audio {

namespace {

using Microsoft::WRL::ComPtr;

// COM init per il thread chiamante (idempotente per design WASAPI).
struct ComInit
{
    HRESULT hr;
    ComInit()  { hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
};

std::wstring getDeviceId(IMMDevice* dev)
{
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return {};
    std::wstring out = id;
    ::CoTaskMemFree(id);
    return out;
}

std::wstring getFriendlyName(IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return L"<unknown>";
    PROPVARIANT v;
    ::PropVariantInit(&v);
    std::wstring name = L"<unknown>";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v))
        && v.vt == VT_LPWSTR && v.pwszVal)
    {
        name = v.pwszVal;
    }
    ::PropVariantClear(&v);
    return name;
}

void fillMixFormat(IMMDevice* dev, AudioDevice& out)
{
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                             nullptr, &client))) return;

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return;

    out.sample_rate = mix->nSamplesPerSec;
    out.channels    = mix->nChannels;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && mix->cbSize >= 22)
    {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        out.channel_mask = ext->dwChannelMask;
    }
    ::CoTaskMemFree(mix);
}

} // namespace

std::vector<AudioDevice> enumerateDevices(DeviceFlow flow)
{
    ComInit com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE)
        return {};

    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumr))))
        return {};

    const EDataFlow eflow = (flow == DeviceFlow::Render) ? eRender : eCapture;

    ComPtr<IMMDevice> def_dev;
    std::wstring default_id;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eflow, eConsole, &def_dev)))
        default_id = getDeviceId(def_dev.Get());

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumr->EnumAudioEndpoints(eflow, DEVICE_STATE_ACTIVE, &coll)))
        return {};

    UINT count = 0;
    coll->GetCount(&count);

    std::vector<AudioDevice> result;
    result.reserve(count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;

        AudioDevice d;
        d.flow          = flow;
        d.id            = getDeviceId(dev.Get());
        d.friendly_name = getFriendlyName(dev.Get());
        d.is_default    = !d.id.empty() && d.id == default_id;
        fillMixFormat(dev.Get(), d);
        result.push_back(std::move(d));
    }
    return result;
}

DeviceList enumerateAllDevices()
{
    return { enumerateDevices(DeviceFlow::Render),
             enumerateDevices(DeviceFlow::Capture) };
}

} // namespace mixer::audio
