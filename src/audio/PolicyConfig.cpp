#include "PolicyConfig.h"

#include <Inspectable.h>
#include <audioclient.h>
#include <hstring.h>
#include <mmreg.h>
#include <propsys.h>
#include <propkeydef.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>

#pragma comment(lib, "runtimeobject.lib")

namespace mixer::audio::policy {

namespace {

using Microsoft::WRL::ComPtr;

// IAudioPolicyConfigFactory: interfaccia Windows non documentata, ricavata
// dal lavoro di SoundSwitch/EarTrumpet/AudioRouter. Eredita da IInspectable
// (WinRT). I metodi __incomplete__ servono solo a riempire la vtable nelle
// posizioni corrette  i nomi reali sono noti ma non ci servono. L'unico
// metodo che ci interessa  SetPersistedDefaultAudioEndpoint.
class DECLSPEC_UUID("AB3D4648-E242-459F-B02F-541C70306324")
    IAudioPolicyConfigFactory : public IInspectable
{
public:
    virtual HRESULT __stdcall __incomplete__add_CtxVolumeChange()              = 0;
    virtual HRESULT __stdcall __incomplete__remove_CtxVolumeNotification()      = 0;
    virtual HRESULT __stdcall __incomplete__add_RingerVibrateStateChanged()     = 0;
    virtual HRESULT __stdcall __incomplete__remove_RingerVibrateStateChange()   = 0;
    virtual HRESULT __stdcall __incomplete__add_RingerMuteStateChanged()        = 0;
    virtual HRESULT __stdcall __incomplete__remove_RingerMuteStateChange()      = 0;
    virtual HRESULT __stdcall __incomplete__SetVolumeGroupGainForId()           = 0;
    virtual HRESULT __stdcall __incomplete__GetVolumeGroupGainForId()           = 0;
    virtual HRESULT __stdcall __incomplete__GetActiveVolumeGroupForEndpointId() = 0;
    virtual HRESULT __stdcall __incomplete__GetVolumeGroupsForEndpoint()        = 0;
    virtual HRESULT __stdcall __incomplete__GetCurrentVolumeContext()           = 0;
    virtual HRESULT __stdcall __incomplete__SetVolumeGroupMuteForId()           = 0;
    virtual HRESULT __stdcall __incomplete__GetVolumeGroupMuteForId()           = 0;
    virtual HRESULT __stdcall __incomplete__SetRingerVibrateState()             = 0;
    virtual HRESULT __stdcall __incomplete__GetRingerVibrateState()             = 0;
    virtual HRESULT __stdcall __incomplete__SetRingerMuteState()                = 0;
    virtual HRESULT __stdcall __incomplete__GetRingerMuteState()                = 0;
    virtual HRESULT __stdcall __incomplete__GetAvailableInputDevicesForGroup()  = 0;
    virtual HRESULT __stdcall __incomplete__GetAvailableOutputDevicesForGroup() = 0;
    virtual HRESULT __stdcall __incomplete__SetVolumeGroupForCommunication()    = 0;
    virtual HRESULT __stdcall __incomplete__add_StreamFactoryDeviceCreated()    = 0;
    virtual HRESULT __stdcall __incomplete__remove_StreamFactoryDeviceCreated() = 0;
    virtual HRESULT __stdcall ClearAllPersistedApplicationDefaultEndpoints()    = 0;
    virtual HRESULT __stdcall SetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING deviceId)           = 0;
    virtual HRESULT __stdcall GetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING* deviceId)          = 0;
};

struct ScopedRoInit
{
    HRESULT hr;
    bool    owns_init = false;
    ScopedRoInit()
    {
        hr = ::RoInitialize(RO_INIT_MULTITHREADED);
        if (SUCCEEDED(hr)) owns_init = true;
        else if (hr == RPC_E_CHANGED_MODE) { hr = S_OK; owns_init = false; }
    }
    ~ScopedRoInit() { if (owns_init) ::RoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr); }
};

} // namespace

// Helper isolato: invoca SetPersistedDefaultAudioEndpoint dentro un __try/
// __except. Se la vtable dell'interfaccia non documentata cambia tra build
// di Windows (es. 26200) e il metodo non esiste a quell'offset, prendiamo
// l'access violation, NON la propaghiamo al chiamante, ritorniamo E_FAIL.
// Questa funzione  POD-only (nessun oggetto C++ con destructor) per
// compatibilit con il modello di eccezioni /EHsc di default.
static HRESULT call_set_persisted_seh(IAudioPolicyConfigFactory* factory,
                                      UINT processId,
                                      EDataFlow flow,
                                      ERole role,
                                      HSTRING deviceId)
{
    __try {
        return factory->SetPersistedDefaultAudioEndpoint(
            processId, flow, role, deviceId);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

bool setAppDefaultEndpoint(uint32_t processId,
                           const std::wstring& device_id,
                           EDataFlow flow,
                           ERole role)
{
    ScopedRoInit ro;
    if (!ro.ok()) return false;

    const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfigFactory";
    HSTRING classNameHstring = nullptr;
    if (FAILED(::WindowsCreateString(className, (UINT32)wcslen(className),
                                     &classNameHstring)))
        return false;

    ComPtr<IAudioPolicyConfigFactory> factory;
    HRESULT hr = ::RoGetActivationFactory(classNameHstring,
                                          __uuidof(IAudioPolicyConfigFactory),
                                          (void**)factory.GetAddressOf());
    ::WindowsDeleteString(classNameHstring);

    if (FAILED(hr) || !factory)
    {
        // Su alcune build Windows il class name "AudioPolicyConfigFactory"
        // non esiste e bisogna usare la variante senza il suffisso "Factory".
        const wchar_t* alt = L"Windows.Media.Internal.AudioPolicyConfig";
        if (FAILED(::WindowsCreateString(alt, (UINT32)wcslen(alt),
                                         &classNameHstring)))
            return false;
        hr = ::RoGetActivationFactory(classNameHstring,
                                      __uuidof(IAudioPolicyConfigFactory),
                                      (void**)factory.GetAddressOf());
        ::WindowsDeleteString(classNameHstring);
        if (FAILED(hr) || !factory) return false;
    }

    HSTRING devHstring = nullptr;
    if (!device_id.empty())
    {
        if (FAILED(::WindowsCreateString(device_id.c_str(),
                                         (UINT32)device_id.size(),
                                         &devHstring)))
            return false;
    }

    // Chiamata protetta da SEH: se la vtable non corrisponde su questa build
    // di Windows e l'accesso al metodo causa AV, recuperiamo e ritorniamo false.
    hr = call_set_persisted_seh(factory.Get(), processId, flow, role, devHstring);

    if (devHstring) ::WindowsDeleteString(devHstring);
    return SUCCEEDED(hr);
}

bool setAppDefaultEndpointAllRoles(uint32_t processId,
                                   const std::wstring& device_id,
                                   EDataFlow flow)
{
    const bool ok_console = setAppDefaultEndpoint(processId, device_id, flow, eConsole);
    const bool ok_mm      = setAppDefaultEndpoint(processId, device_id, flow, eMultimedia);
    // Communications  spesso gestito diversamente; tentiamo, fallimento  ok.
    setAppDefaultEndpoint(processId, device_id, flow, eCommunications);
    return ok_console || ok_mm;
}

// ---- IPolicyConfig (interfaccia stabile dal 2007 per il default di sistema) ----

namespace {

// CLSID di CPolicyConfigClient (registrato dalla DLL audiosrv).
// Stabile su tutte le versioni di Windows dal Vista.
const CLSID CLSID_CPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e,
    { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 }
};

// IID di IPolicyConfig (la versione "classica" stabile).
class DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8")
    IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT __stdcall GetMixFormat(LPCWSTR, WAVEFORMATEX**)            = 0;
    virtual HRESULT __stdcall GetDeviceFormat(LPCWSTR, INT, WAVEFORMATEX**)    = 0;
    virtual HRESULT __stdcall ResetDeviceFormat(LPCWSTR)                       = 0;
    virtual HRESULT __stdcall SetDeviceFormat(LPCWSTR, WAVEFORMATEX*,
                                              WAVEFORMATEX*)                   = 0;
    virtual HRESULT __stdcall GetProcessingPeriod(LPCWSTR, INT, INT64*,
                                                  INT64*)                       = 0;
    virtual HRESULT __stdcall SetProcessingPeriod(LPCWSTR, INT64*)             = 0;
    virtual HRESULT __stdcall GetShareMode(LPCWSTR, INT*)                      = 0;
    virtual HRESULT __stdcall SetShareMode(LPCWSTR, INT*)                      = 0;
    virtual HRESULT __stdcall GetPropertyValue(LPCWSTR, const PROPERTYKEY&,
                                               PROPVARIANT*)                    = 0;
    virtual HRESULT __stdcall SetPropertyValue(LPCWSTR, const PROPERTYKEY&,
                                               PROPVARIANT*)                    = 0;
    virtual HRESULT __stdcall SetDefaultEndpoint(LPCWSTR wszDeviceId,
                                                 ERole eRole)                   = 0;
    virtual HRESULT __stdcall SetEndpointVisibility(LPCWSTR, INT)              = 0;
};

struct ScopedComApt
{
    HRESULT hr;
    ScopedComApt()
    {
        hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    ~ScopedComApt() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

} // namespace

bool setSystemDefaultEndpoint(const std::wstring& device_id, ERole role)
{
    if (device_id.empty()) return false;
    ScopedComApt com;
    if (!com.ok()) return false;

    ComPtr<IPolicyConfig> pc;
    HRESULT hr = ::CoCreateInstance(CLSID_CPolicyConfigClient, nullptr,
                                    CLSCTX_ALL, __uuidof(IPolicyConfig),
                                    (void**)pc.GetAddressOf());
    if (FAILED(hr) || !pc) return false;

    hr = pc->SetDefaultEndpoint(device_id.c_str(), role);
    return SUCCEEDED(hr);
}

bool setSystemDefaultEndpointAllRoles(const std::wstring& device_id)
{
    const bool a = setSystemDefaultEndpoint(device_id, eConsole);
    const bool b = setSystemDefaultEndpoint(device_id, eMultimedia);
    setSystemDefaultEndpoint(device_id, eCommunications);  // best-effort
    return a || b;
}

// ---- IAudioPolicyConfigFactory (per-app, undocumented WinRT) ---------------

std::wstring getAppDefaultEndpoint(uint32_t processId, EDataFlow flow, ERole role)
{
    ScopedRoInit ro;
    if (!ro.ok()) return {};

    const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfigFactory";
    HSTRING classNameHstring = nullptr;
    if (FAILED(::WindowsCreateString(className, (UINT32)wcslen(className),
                                     &classNameHstring)))
        return {};

    ComPtr<IAudioPolicyConfigFactory> factory;
    HRESULT hr = ::RoGetActivationFactory(classNameHstring,
                                          __uuidof(IAudioPolicyConfigFactory),
                                          (void**)factory.GetAddressOf());
    ::WindowsDeleteString(classNameHstring);

    if (FAILED(hr) || !factory)
    {
        const wchar_t* alt = L"Windows.Media.Internal.AudioPolicyConfig";
        if (FAILED(::WindowsCreateString(alt, (UINT32)wcslen(alt), &classNameHstring)))
            return {};
        hr = ::RoGetActivationFactory(classNameHstring,
                                      __uuidof(IAudioPolicyConfigFactory),
                                      (void**)factory.GetAddressOf());
        ::WindowsDeleteString(classNameHstring);
        if (FAILED(hr) || !factory) return {};
    }

    HSTRING devHstring = nullptr;
    hr = factory->GetPersistedDefaultAudioEndpoint(processId, flow, role, &devHstring);
    if (FAILED(hr) || !devHstring) return {};

    UINT32 len = 0;
    const wchar_t* buf = ::WindowsGetStringRawBuffer(devHstring, &len);
    std::wstring result = (buf && len > 0) ? std::wstring(buf, len) : std::wstring();
    ::WindowsDeleteString(devHstring);
    return result;
}

} // namespace mixer::audio::policy
