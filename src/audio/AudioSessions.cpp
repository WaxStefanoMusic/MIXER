#include "AudioSessions.h"
#include "PolicyConfig.h"
#include "WasapiCommon.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <Psapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <unordered_map>

namespace mixer::audio {

namespace {

std::wstring getProcessName(DWORD pid)
{
    if (pid == 0) return L"system_sounds";
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = ::QueryFullProcessImageNameW(h, 0, buf, &size);
    ::CloseHandle(h);
    if (!ok) return L"";
    std::wstring path = buf;
    auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path = path.substr(slash + 1);
    return path;
}

std::wstring getEndpointFriendly(IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return L"";
    PROPVARIANT v; ::PropVariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v))
        && v.vt == VT_LPWSTR && v.pwszVal)
        name = v.pwszVal;
    ::PropVariantClear(&v);
    return name;
}

} // namespace

std::vector<AppSession> enumerateAppSessions()
{
    std::vector<AppSession> result;

    ScopedComInit com;
    if (!com.ok()) return result;

    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumr))))
        return result;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
        return result;

    UINT count = 0;
    coll->GetCount(&count);

    // Mappa pid -> indice in result, per deduplicare sessioni di stessa app su pi device.
    std::unordered_map<uint64_t, size_t> pid_index;

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;

        LPWSTR devId = nullptr;
        dev->GetId(&devId);
        std::wstring devIdStr = devId ? devId : L"";
        if (devId) ::CoTaskMemFree(devId);

        std::wstring devName = getEndpointFriendly(dev.Get());

        ComPtr<IAudioSessionManager2> sessMgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                 nullptr, &sessMgr))) continue;

        ComPtr<IAudioSessionEnumerator> sessEnum;
        if (FAILED(sessMgr->GetSessionEnumerator(&sessEnum))) continue;

        int sessCount = 0;
        sessEnum->GetCount(&sessCount);

        for (int s = 0; s < sessCount; ++s)
        {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessEnum->GetSession(s, &ctrl))) continue;

            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl.As(&ctrl2))) continue;

            const bool isSysSounds = ctrl2->IsSystemSoundsSession() == S_OK;

            DWORD pid = 0;
            ctrl2->GetProcessId(&pid);
            if (!isSysSounds && pid == 0) continue;

            // Skippa anche la nostra app per non mostrarci a noi stessi.
            if (pid == ::GetCurrentProcessId()) continue;

            AppSession session;
            session.process_id        = pid;
            session.is_system_sounds  = isSysSounds;
            session.process_name      = isSysSounds ? L"system_sounds"
                                                     : getProcessName(pid);

            LPWSTR displayName = nullptr;
            if (SUCCEEDED(ctrl->GetDisplayName(&displayName)) && displayName)
            {
                if (*displayName) session.display_name = displayName;
                ::CoTaskMemFree(displayName);
            }
            if (session.display_name.empty())
            {
                // fallback: rimuovi .exe dal process name per leggibilit
                session.display_name = session.process_name;
                const std::wstring ext = L".exe";
                if (session.display_name.size() > ext.size()
                    && session.display_name.compare(
                        session.display_name.size() - ext.size(),
                        ext.size(), ext) == 0)
                    session.display_name.erase(
                        session.display_name.size() - ext.size());
            }

            session.current_endpoint_id   = devIdStr;
            session.current_endpoint_name = devName;
            // override_endpoint_id viene calcolato lato UI confrontando con il
            // device predefinito di sistema. Non chiamiamo l'API undocumented
            // policy::getAppDefaultEndpoint() qui perch sulla build Windows
            // 11 26200 il suo vtable layout pu causare access violation
            // (gli offset cambiano tra build).
            session.override_endpoint_id.clear();

            // Peak meter di sessione (post-volume).
            ComPtr<IAudioMeterInformation> meter;
            if (SUCCEEDED(ctrl.As(&meter)))
                meter->GetPeakValue(&session.peak);

            AudioSessionState state = AudioSessionStateInactive;
            ctrl->GetState(&state);
            session.is_playing = (state == AudioSessionStateActive);

            // Deduplica per pid: se gi visto, aggiorna solo se questo  attivo.
            const uint64_t key = (uint64_t)pid | ((uint64_t)isSysSounds << 32);
            auto it = pid_index.find(key);
            if (it == pid_index.end())
            {
                pid_index[key] = result.size();
                result.push_back(std::move(session));
            }
            else
            {
                auto& existing = result[it->second];
                if (session.is_playing && !existing.is_playing)
                    existing = std::move(session);
                else if (session.peak > existing.peak)
                {
                    existing.peak = session.peak;
                    if (session.is_playing) existing.is_playing = true;
                }
            }
        }
    }

    return result;
}

} // namespace mixer::audio
