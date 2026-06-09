#pragma once

// ============================================================================
//  REGOLA FERREA (NON VIOLARE)
//  MIXER non deve MAI modificare nomi o icone dei dispositivi audio di Windows
//  su nessun PC. Quindi: leggere le proprieta' device solo in lettura
//  (STGM_READ + GetValue); MAI STGM_READWRITE, MAI IPropertyStore::SetValue,
//  MAI IPolicyConfig::SetPropertyValue, MAI scrivere PKEY_Device_FriendlyName
//  o PKEY_DeviceClass_IconPath. L'unica modifica di sistema consentita qui e'
//  cambiare il device PREDEFINITO (SetDefaultEndpoint), che NON tocca
//  nomi/icone ed e' attivata solo su azione esplicita dell'utente.
// ============================================================================

#include <Windows.h>
#include <mmdeviceapi.h>
#include <cstdint>
#include <string>

namespace mixer::audio::policy {

// Imposta l'uscita audio "persisted" per un'app (override per-processo).
// Equivale a quello che fa Windows  Impostazioni  Audio  Mixer volume
// (cambia l'uscita di una singola app).
//
// device_id: PnP id ottenuto da IMMDevice::GetId() (formato
//            "{0.0.0.00000000}.{guid}"). Passa stringa vuota per RESETTARE
//            l'override e tornare al device predefinito di sistema.
// flow     : eRender per uscite, eCapture per ingressi.
// role     : eConsole (multimedia generale), eMultimedia, eCommunications.
//            Tipicamente vuoi eConsole e eMultimedia entrambi  vedi
//            setAppDefaultEndpointBothRoles().
//
// Ritorna true se l'operazione  riuscita.
// Nota: usa IAudioPolicyConfigFactory (interfaccia Windows non documentata,
// stessa tecnica di SoundSwitch/EarTrumpet). Funziona su Windows 10 1809+
// e Windows 11.
bool setAppDefaultEndpoint(uint32_t processId,
                           const std::wstring& device_id,
                           EDataFlow flow = eRender,
                           ERole role = eConsole);

// Imposta sia eConsole sia eMultimedia. Da preferire perch alcune app
// usano un role e altre l'altro, e Windows li gestisce separatamente.
bool setAppDefaultEndpointAllRoles(uint32_t processId,
                                   const std::wstring& device_id,
                                   EDataFlow flow = eRender);

// Ritorna l'eventuale device_id "override" attualmente impostato per quella
// app sul ruolo dato. Se non c' override (app usa il predefinito di sistema)
// ritorna stringa vuota.
//
// AVVERTENZA: chiamata pu causare access violation su build Windows recenti
// (26200+) per vtable instabile dell'API undocumented. Usare con SEH wrapper
// o evitare. Mantenuto per completezza.
std::wstring getAppDefaultEndpoint(uint32_t processId,
                                   EDataFlow flow = eRender,
                                   ERole role = eMultimedia);

// -- API STABILE (IPolicyConfig, stabile dal 2007) ---------------------------
// Cambia il device PREDEFINITO DI SISTEMA per il ruolo dato.
// device_id: PnP id di un endpoint render (es. da IMMDevice::GetId()).
// role     : eConsole, eMultimedia, eCommunications.
//
// Si appoggia all'interfaccia IPolicyConfig (undocumented MA stabile su tutte
// le versioni di Windows da Vista in poi). Usata da NirSoft SoundVolumeView,
// AudioSwitch e tools simili. Niente WinRT, niente IInspectable, niente
// vtable mismatch.
bool setSystemDefaultEndpoint(const std::wstring& device_id,
                              ERole role = eMultimedia);

// Applica a tutti i ruoli (eConsole + eMultimedia + eCommunications). Da
// preferire: alcune app usano un ruolo, altre un altro, e Windows tratta
// "Predefinito" e "Predefinito comunicazioni" come device separati.
bool setSystemDefaultEndpointAllRoles(const std::wstring& device_id);

} // namespace mixer::audio::policy
