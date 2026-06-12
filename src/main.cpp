// MIXER  Dear ImGui based audio mixer for Windows.
//
// Modalit di esecuzione:
//   mixer.exe              GUI normale
//   mixer.exe --smoke      Smoke test: enumera device + apre/chiude default
//                          render per 500ms + roundtrip save/load di una
//                          MixerCfg di prova. Scrive report a ".\smoke.log".

#include <windows.h>
#include <d3d11.h>
#include <audioclient.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "audio/DeviceEnumerator.h"
#include "audio/WasapiRenderStream.h"
#include "audio/AudioEngine.h"
#include "audio/AudioSessions.h"
#include "audio/PolicyConfig.h"
#include "config/MixerConfig.h"
#include "update/Updater.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Versione iniettata da CMake (target_compile_definitions). Fallback per build
// IDE che non passano il define.
#ifndef MIXER_VERSION
#define MIXER_VERSION "0.0.0-dev"
#endif

// ---- Stato globale Win32/DX11 ----------------------------------------------

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static UINT                     g_ResizeWidth = 0;
static UINT                     g_ResizeHeight = 0;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---- Helpers ---------------------------------------------------------------

namespace {

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          out.data(), n, nullptr, nullptr);
    return out;
}

// Directory dell'eseguibile (con backslash finale). Usata per trovare i
// preset accanto all'exe  fondamentale per l'installazione su altri PC,
// dove il path "B:\MIXER\..." non esiste.
std::wstring exeDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".\\";
    std::wstring p(buf, n);
    const auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".\\";
    return p.substr(0, slash + 1);
}

// UTF-8 -> wide (inverso di narrow()).
std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// ---- Dati utente: Documenti\MIXER ------------------------------------------
// Preset e impostazioni vivono in Documenti\MIXER (scrivibile senza admin,
// sopravvive ad aggiornamenti/disinstallazioni dell'app), NON in Program Files.
std::wstring documentsMixerDir()
{
    wchar_t buf[MAX_PATH] = L"";
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr,
                                     SHGFP_TYPE_CURRENT, buf)))
    {
        std::wstring d(buf);
        if (!d.empty() && d.back() != L'\\') d += L'\\';
        return d + L"MIXER\\";
    }
    return exeDir();  // fallback estremo
}

std::wstring presetsDir()   { return documentsMixerDir() + L"Preset\\"; }
std::wstring settingsPath() { return documentsMixerDir() + L"settings.ini"; }

void ensureDir(const std::wstring& dir)
{
    if (!dir.empty()) ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}

// Nome visualizzato del preset: "Default" se vuoto, altrimenti il nome file
// senza estensione.
std::wstring presetDisplayName(const std::wstring& path)
{
    if (path.empty()) return L"Default";
    auto slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    return name;
}

// Prima esecuzione: crea Documenti\MIXER\Preset e, se vuota, ci copia i preset
// di esempio installati accanto all'exe ({app}\Preset Salvati).
void seedPresetsFromInstall()
{
    ensureDir(presetsDir());
    const std::wstring dst = presetsDir();

    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dst + L"*.mxp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { ::FindClose(h); return; }  // gia' popolata

    const std::wstring src = exeDir() + L"Preset Salvati\\";
    h = ::FindFirstFileW((src + L"*.mxp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ::CopyFileW((src + fd.cFileName).c_str(),
                    (dst + fd.cFileName).c_str(), TRUE /*non sovrascrivere*/);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// settings.ini: riga "default_preset=<path>". Vuoto = preset vergine.
std::wstring loadDefaultPresetSetting()
{
    FILE* f = nullptr;
    if (::_wfopen_s(&f, settingsPath().c_str(), L"rb") != 0 || !f) return {};
    std::string content;
    char buf[1024]; size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    ::fclose(f);

    const std::string key = "default_preset=";
    auto p = content.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    auto end = content.find_first_of("\r\n", p);
    std::string val = content.substr(p, end == std::string::npos ? std::string::npos : end - p);
    return widen(val);
}

void saveDefaultPresetSetting(const std::wstring& path)
{
    ensureDir(documentsMixerDir());
    FILE* f = nullptr;
    if (::_wfopen_s(&f, settingsPath().c_str(), L"wb") != 0 || !f) return;
    std::string line = "default_preset=" + narrow(path) + "\n";
    ::fwrite(line.data(), 1, line.size(), f);
    ::fclose(f);
}

// Disegna una spunta verde "a mano" (indipendente dal font) di lato ~h px,
// con l'angolo alto-sinistra in p. Disegna sul draw list della finestra, quindi
// puo' essere sovrapposta a un pulsante.
void drawGreenCheckAt(ImVec2 p, float h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = IM_COL32(80, 220, 80, 255);
    dl->AddLine(ImVec2(p.x + h * 0.12f, p.y + h * 0.55f),
                ImVec2(p.x + h * 0.40f, p.y + h * 0.82f), col, 2.5f);
    dl->AddLine(ImVec2(p.x + h * 0.40f, p.y + h * 0.82f),
                ImVec2(p.x + h * 0.88f, p.y + h * 0.20f), col, 2.5f);
}

// Mappatura nomi friendly  display-name umani per i device noti.
// Windows restituisce stringhe lunghissime come "Game Capture 4K60 Pro MK.2
// Audio (Game Capture 4K60 Pro MK.2)" che intasano la UI; le accorciamo a
// nomi riconoscibili. Match per sottostringa sul friendly_name fornito da
// Windows. Se nessun pattern matcha, restituiamo il nome cos com'.
std::string shortDeviceName(const std::wstring& friendly_name)
{
    std::string s = narrow(friendly_name);
    if (s.empty()) return s;

    // Elgato 4K Pro / 4K60 Pro / 4K60 Pro MK.2  tutti varianti dello stesso
    // hardware. "Game Capture" copre quasi tutta la serie.
    if (s.find("Game Capture") != std::string::npos)
        return "Elgato 4K Pro";

    // VB-CABLE  letter-coded
    if (s.find("VB-Audio Cable A") != std::string::npos) return "VB-CABLE A";
    if (s.find("VB-Audio Cable B") != std::string::npos) return "VB-CABLE B";
    if (s.find("VB-Audio Cable C") != std::string::npos) return "VB-CABLE C";
    if (s.find("VB-Audio Cable D") != std::string::npos) return "VB-CABLE D";
    if (s.find("VB-Audio Virtual Cable") != std::string::npos)
        return "VB-CABLE (virtual)";

    // Voicemeeter: strippa il suffisso "(VB-Audio Voicemeeter VAIO)" lasciando
    // il sotto-bus specifico (Voicemeeter Input, AUX, In 2, Out A3, ecc.).
    if (s.find("Voicemeeter") != std::string::npos)
    {
        auto cut = s.find(" (VB-Audio");
        if (cut != std::string::npos) return s.substr(0, cut);
        return s;
    }

    // NVIDIA HD Audio: tieni il nome del display/monitor (utente l'ha
    // rinominato spesso) MA aggiungi marker [HDMI] per distinguere dai
    // VB-CABLE/altri device con stesso nome friendly.
    // Esempi: "PG32UCDMR (NVIDIA HD Audio)" -> "PG32UCDMR [HDMI]"
    //         "CABLE-C Input (NVIDIA HD Audio)" -> "CABLE-C Input [HDMI]"
    if (s.find("NVIDIA") != std::string::npos
        && (s.find("High Definition Audio") != std::string::npos
            || s.find("high definition audio") != std::string::npos
            || s.find("HDMI") != std::string::npos))
    {
        auto cut = s.find(" (NVIDIA");
        if (cut != std::string::npos) return s.substr(0, cut) + "  [HDMI]";
        return s + "  [HDMI]";
    }

    // Razer Nommo Pro: lascia il prefisso "Altoparlanti" perch utile.
    if (s.find("Razer Nommo Pro") != std::string::npos)
        return "Razer Nommo Pro";

    // Realtek USB Audio: prova a distinguere uscita/ingresso. Il nome
    // 'Altoparlanti' indica un endpoint di riproduzione; gli altri sono
    // tipicamente input (mic / line-in).
    if (s.find("Realtek USB Audio") != std::string::npos)
    {
        if (s.find("Altoparlanti") != std::string::npos)
            return "Realtek USB (uscita)";
        if (s.find("Digital") != std::string::npos)
            return "Realtek USB (digital out)";
        // Altrimenti  un endpoint di ingresso; ritorna nome come fornito da
        // Windows (l'utente l'ha verosimilmente rinominato per il microfono).
        auto cut = s.find(" (Realtek");
        if (cut != std::string::npos) return s.substr(0, cut) + "  Realtek mic";
        return s;
    }

    return s;
}

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
float linToDb(float l)  { return 20.0f * std::log10(std::max(l, 1e-6f)); }

const char* hresultMessage(long hr)
{
    switch ((HRESULT)hr)
    {
    case 0:                                 return "OK";
    case AUDCLNT_E_NOT_INITIALIZED:         return "Audio client non inizializzato.";
    case AUDCLNT_E_ALREADY_INITIALIZED:     return "Audio client gi inizializzato.";
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE:     return "Tipo di endpoint non corretto.";
    case AUDCLNT_E_DEVICE_INVALIDATED:      return "Il device  stato scollegato o disattivato.";
    case AUDCLNT_E_NOT_STOPPED:             return "Lo stream non  fermo.";
    case AUDCLNT_E_BUFFER_TOO_LARGE:        return "Buffer richiesto troppo grande.";
    case AUDCLNT_E_OUT_OF_ORDER:            return "Operazione fuori sequenza.";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:      return "Il device non supporta il formato float 32-bit richiesto.";
    case AUDCLNT_E_INVALID_DEVICE_PERIOD:   return "Periodo del buffer non valido.";
    case AUDCLNT_E_INVALID_SIZE:            return "Dimensione del buffer non valida.";
    case AUDCLNT_E_DEVICE_IN_USE:           return "Device gi in uso (modalit exclusive).";
    case AUDCLNT_E_BUFFER_OPERATION_PENDING:return "Operazione su buffer in attesa.";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:  return "Creazione endpoint fallita.";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:     return "Servizio audio di Windows non in esecuzione.";
    case E_ACCESSDENIED:                    return "Accesso negato al device.";
    case E_OUTOFMEMORY:                     return "Memoria insufficiente.";
    case E_INVALIDARG:                      return "Argomento non valido.";
    case E_POINTER:                         return "Puntatore non valido.";
    case E_FAIL:                            return "Errore generico.";
    case REGDB_E_CLASSNOTREG:               return "Classe COM non registrata (driver audio assente?).";
    default:                                return "Errore sconosciuto.";
    }
}

void enableDpiAwareness()
{
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL (WINAPI* PFN)(DPI_AWARENESS_CONTEXT);
    auto pfn = (PFN)::GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (pfn) pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// DPI di sistema (chiamabile prima della creazione della finestra).
UINT systemDpi()
{
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (u32)
    {
        typedef UINT (WINAPI* PFN)(void);
        auto pfn = (PFN)::GetProcAddress(u32, "GetDpiForSystem");
        if (pfn)
        {
            UINT d = pfn();
            if (d) return d;
        }
    }
    HDC hdc = ::GetDC(nullptr);
    UINT dpi = ::GetDeviceCaps(hdc, LOGPIXELSX);
    ::ReleaseDC(nullptr, hdc);
    return dpi ? dpi : 96;
}

// DPI del monitor su cui  attualmente il cursore. Da chiamare PRIMA di
// CreateWindow per ottenere il DPI effettivo del display target, anche
// quando systemDpi() ritorna 96 perch SetProcessDpiAwarenessContext non
// ha ancora "abilitato" l'awareness lato GetDpiForSystem.
UINT cursorMonitorDpi()
{
    POINT pt;
    if (!::GetCursorPos(&pt)) return systemDpi();
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) return systemDpi();
    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (!shcore) return systemDpi();
    typedef HRESULT (WINAPI* PFN)(HMONITOR, int, UINT*, UINT*);
    auto pfn = (PFN)::GetProcAddress(shcore, "GetDpiForMonitor");
    UINT dx = 0, dy = 0;
    if (pfn) pfn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy);
    ::FreeLibrary(shcore);
    return (dx != 0) ? dx : systemDpi();
}

UINT windowDpi(HWND hwnd)
{
    if (!hwnd) return systemDpi();
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (u32)
    {
        typedef UINT (WINAPI* PFN)(HWND);
        auto pfn = (PFN)::GetProcAddress(u32, "GetDpiForWindow");
        if (pfn)
        {
            UINT d = pfn(hwnd);
            if (d) return d;
        }
    }
    return systemDpi();
}

// AdjustWindowRectExForDpi se disponibile, altrimenti AdjustWindowRectEx.
void adjustWindowForDpi(LPRECT rc, DWORD style, BOOL has_menu,
                        DWORD ex_style, UINT dpi)
{
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (u32)
    {
        typedef BOOL (WINAPI* PFN)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto pfn = (PFN)::GetProcAddress(u32, "AdjustWindowRectExForDpi");
        if (pfn) { pfn(rc, style, has_menu, ex_style, dpi); return; }
    }
    ::AdjustWindowRectEx(rc, style, has_menu, ex_style);
}

// File dialog wrappers ------------------------------------------------------

std::wstring openPresetDialog(HWND owner)
{
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"MIXER preset (*.mxp)\0*.mxp\0Tutti i file\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"mxp";
    const std::wstring initDir = presetsDir();
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!::GetOpenFileNameW(&ofn)) return {};
    return buf;
}

std::wstring savePresetDialog(HWND owner, const std::wstring& suggested)
{
    wchar_t buf[MAX_PATH] = L"";
    if (!suggested.empty())
    {
        std::wstring s = suggested;
        if (s.size() >= MAX_PATH) s.resize(MAX_PATH - 1);
        std::copy(s.begin(), s.end(), buf);
        buf[s.size()] = L'\0';
    }
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"MIXER preset (*.mxp)\0*.mxp\0Tutti i file\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"mxp";
    const std::wstring initDir = presetsDir();
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!::GetSaveFileNameW(&ofn)) return {};
    return buf;
}

// ---- Test-tone state -------------------------------------------------------

struct TestToneState
{
    std::atomic<float> hz{440.0f};
    std::atomic<float> gain_lin{0.1259f}; // -18 dB
    double             phase = 0.0;
};

void fillSineTone(TestToneState& st, float* dst, uint32_t frames,
                  uint32_t channels, uint32_t sample_rate)
{
    const float hz = st.hz.load(std::memory_order_relaxed);
    const float g  = st.gain_lin.load(std::memory_order_relaxed);
    const double inc = (2.0 * M_PI * (double)hz) / (double)sample_rate;
    double ph = st.phase;
    for (uint32_t f = 0; f < frames; ++f)
    {
        const float s = (float)std::sin(ph) * g;
        ph += inc;
        if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
        for (uint32_t c = 0; c < channels; ++c)
            dst[f * channels + c] = s;
    }
    st.phase = ph;
}

// ---- UI state cluster ------------------------------------------------------

struct UiState
{
    bool show_welcome     = false;  // niente popup all'apertura, riapribile da Aiuto -> Guida rapida
    bool show_mixer       = true;
    bool show_devices     = false;
    bool show_about       = false;
    // (la Routing app  inline nel pannello mixer, niente popup)

    // Impostazioni globali audio engine (modificabili dalla toolbar del mixer).
    // Default 3 ms = minimo richiedibile (la latenza pi bassa possibile).
    // NB: WASAPI shared arrotonda comunque al periodo motore ~10 ms, quindi
    // la latenza reale non scende sotto ~35 ms in cuffia senza Exclusive mode.
    // Se senti click in cuffia alza a 8-15 ms (la registrazione Elgato in
    // bypass non  comunque mai influenzata dal buffer).
    int  buffer_ms = 3;
    // Se true (default), i fader dB e il soft-clip sono attivi  i fader
    // di strip/bus regolano davvero il volume. Togliere la spunta fa
    // diventare il mixer un passthrough puro (audio invariato).
    bool enable_dbfs = true;
    // Se true, l'engine tenta WASAPI Exclusive mode (latenza ~3-5 ms invece
    // di ~10 ms del motore shared). Il device viene monopolizzato da MIXER.
    // Applicato al prossimo Avvia mixer.
    bool exclusive_mode = false;
    // Se true, l'engine tenta IAudioClient3 low-latency SHARED (periodo ~3 ms,
    // device NON monopolizzato, funziona anche col loopback). Mutuamente
    // esclusivo con exclusive_mode.
    bool lowlat_mode = false;

    std::wstring current_preset_path;  // preset attualmente caricato ("" = vergine/Default)
    std::wstring default_preset_path;  // preset caricato all'avvio ("" = vergine), in settings.ini
    std::string  status_message;       // breve messaggio mostrato sopra la status bar
    double       status_until_time = 0.0;
};

// Stato del pannello "Routing app" (sessioni audio Windows).
struct AppRoutingState
{
    std::vector<mixer::audio::AppSession> sessions;
    double last_refresh_time = -1.0;
    bool   auto_refresh = true;
    // Settato quando il default di sistema cambia: il main loop
    // ri-enumera i device per aggiornare il flag is_default.
    bool   device_refresh_needed = false;
};

// Stato decay-visivo dei peak meter (separato da UiState perch transitorio).
// Aggiornato ogni frame dal thread UI leggendo i picchi atomici dall'engine.
struct MeterDisplayState
{
    std::vector<std::array<float, 2>> strip;  // [strip_idx][ch] in lineare 0..1+
    std::vector<std::array<float, 2>> bus;
    // Picco massimo raggiunto (max L/R), lineare. Resettabile dall'utente.
    std::vector<float> strip_hold;
    std::vector<float> bus_hold;

    void resize(size_t ns, size_t nb)
    {
        if (strip.size() != ns) strip.resize(ns, {0.0f, 0.0f});
        if (bus.size()   != nb) bus.resize(nb,   {0.0f, 0.0f});
        if (strip_hold.size() != ns) strip_hold.resize(ns, 0.0f);
        if (bus_hold.size()   != nb) bus_hold.resize(nb,   0.0f);
    }
};

void setStatus(UiState& ui, const std::string& msg, double seconds = 4.0)
{
    ui.status_message = msg;
    ui.status_until_time = ImGui::GetTime() + seconds;
}

void helpMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---- Color palette for buses ----------------------------------------------

ImU32 busColor(int bus_idx, float alpha = 1.0f)
{
    static const ImU32 colors[] = {
        IM_COL32(255, 100, 100, 255),  // 0: rosso
        IM_COL32( 90, 180, 255, 255),  // 1: azzurro
        IM_COL32(120, 230, 130, 255),  // 2: verde
        IM_COL32(255, 220,  90, 255),  // 3: giallo
        IM_COL32(220, 140, 255, 255),  // 4: viola
        IM_COL32(255, 170,  80, 255),  // 5: arancione
    };
    ImU32 c = colors[bus_idx % IM_ARRAYSIZE(colors)];
    if (alpha < 1.0f)
    {
        ImU32 a = (ImU32)(255.0f * alpha) & 0xFFu;
        c = (c & 0x00FFFFFFu) | (a << 24);
    }
    return c;
}

ImVec4 busColorV(int bus_idx)
{
    const ImU32 c = busColor(bus_idx);
    return ImVec4(((c >> 0)  & 0xff) / 255.0f,
                  ((c >> 8)  & 0xff) / 255.0f,
                  ((c >> 16) & 0xff) / 255.0f,
                  ((c >> 24) & 0xff) / 255.0f);
}

} // namespace

// ---- Device selection combo ------------------------------------------------

// Mostra un combobox con le label dei device candidati + un'opzione
// "Non assegnato" in cima. Aggiorna *selected_id e *selected_loopback.
// candidates_render contiene tutti i render device, candidates_capture i capture.
// include_loopback: se true, i render device appaiono come opzioni "loopback".
// Ritorna true se l'utente ha cambiato qualcosa.
static bool deviceComboBox(const char* label_id,
                           const mixer::audio::DeviceList& devs,
                           bool include_capture,
                           bool include_loopback,
                           bool include_render_direct,
                           std::wstring* selected_id,
                           bool* selected_loopback)
{
    // Trova label per la selezione corrente.
    std::string current = "<non assegnato>";
    if (!selected_id->empty())
    {
        bool found = false;
        if (include_render_direct || include_loopback)
        {
            for (const auto& d : devs.render)
            {
                if (d.id == *selected_id)
                {
                    current = shortDeviceName(d.friendly_name);
                    if (selected_loopback && *selected_loopback)
                        current += "  [loopback]";
                    found = true; break;
                }
            }
        }
        if (!found && include_capture)
        {
            for (const auto& d : devs.capture)
            {
                if (d.id == *selected_id) { current = shortDeviceName(d.friendly_name); found = true; break; }
            }
        }
        if (!found) current = "<device non pi disponibile>";
    }

    bool changed = false;

    // Helper: ordina gli indici di una lista device per nome corto
    // (case-insensitive, natural-ish — i numeri si confrontano per valore
    // numerico, es. "In 2" < "In 10").
    auto sortedIndices =
        [](const std::vector<mixer::audio::AudioDevice>& list)
    {
        std::vector<size_t> idx(list.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            const std::string na = shortDeviceName(list[a].friendly_name);
            const std::string nb = shortDeviceName(list[b].friendly_name);
            // Natural compare: confronta carattere per carattere, ma sui
            // run di cifre confronta come numeri.
            size_t i = 0, j = 0;
            while (i < na.size() && j < nb.size())
            {
                const unsigned char ca = (unsigned char)na[i];
                const unsigned char cb = (unsigned char)nb[j];
                if (std::isdigit(ca) && std::isdigit(cb))
                {
                    size_t ea = i, eb = j;
                    while (ea < na.size() && std::isdigit((unsigned char)na[ea])) ++ea;
                    while (eb < nb.size() && std::isdigit((unsigned char)nb[eb])) ++eb;
                    const size_t la = ea - i, lb = eb - j;
                    if (la != lb) return la < lb;
                    for (size_t k = 0; k < la; ++k)
                        if (na[i + k] != nb[j + k]) return na[i + k] < nb[j + k];
                    i = ea; j = eb;
                }
                else
                {
                    const unsigned char la = (unsigned char)std::tolower(ca);
                    const unsigned char lb = (unsigned char)std::tolower(cb);
                    if (la != lb) return la < lb;
                    ++i; ++j;
                }
            }
            return na.size() < nb.size();
        });
        return idx;
    };

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo(label_id, current.c_str()))
    {
        if (ImGui::Selectable("<non assegnato>", selected_id->empty()))
        {
            selected_id->clear();
            if (selected_loopback) *selected_loopback = false;
            changed = true;
        }

        if (include_capture && !devs.capture.empty())
        {
            ImGui::SeparatorText("Input / Microfoni");
            for (size_t i : sortedIndices(devs.capture))
            {
                const auto& d = devs.capture[i];
                std::string lbl = shortDeviceName(d.friendly_name);
                if (d.is_default) lbl += "  [predefinito]";
                ImGui::PushID((int)i);
                const bool sel = (d.id == *selected_id
                                  && (!selected_loopback || !*selected_loopback));
                if (ImGui::Selectable(lbl.c_str(), sel))
                {
                    *selected_id = d.id;
                    if (selected_loopback) *selected_loopback = false;
                    changed = true;
                }
                ImGui::PopID();
            }
        }

        if (include_loopback && !devs.render.empty())
        {
            ImGui::SeparatorText("Output / Cuffie / Impianto Audio");
            for (size_t i : sortedIndices(devs.render))
            {
                const auto& d = devs.render[i];
                std::string lbl = shortDeviceName(d.friendly_name);
                if (d.is_default) lbl += "  [predefinito]";
                ImGui::PushID(10000 + (int)i);
                const bool sel = (d.id == *selected_id
                                  && selected_loopback && *selected_loopback);
                if (ImGui::Selectable(lbl.c_str(), sel))
                {
                    *selected_id = d.id;
                    if (selected_loopback) *selected_loopback = true;
                    changed = true;
                }
                ImGui::PopID();
            }
        }

        if (include_render_direct && !devs.render.empty())
        {
            ImGui::SeparatorText("Output / Cuffie / Impianto Audio");
            for (size_t i : sortedIndices(devs.render))
            {
                const auto& d = devs.render[i];
                std::string lbl = shortDeviceName(d.friendly_name);
                if (d.is_default) lbl += "  [predefinito]";
                ImGui::PushID(20000 + (int)i);
                const bool sel = (d.id == *selected_id
                                  && (!selected_loopback || !*selected_loopback));
                if (ImGui::Selectable(lbl.c_str(), sel))
                {
                    *selected_id = d.id;
                    if (selected_loopback) *selected_loopback = false;
                    changed = true;
                }
                ImGui::PopID();
            }
        }

        ImGui::EndCombo();
    }
    return changed;
}

// ---- Welcome / About / Devices / Test Tone panels --------------------------

static void RenderWelcomePanel(bool* p_open)
{
    if (!*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(680.0f * ImGui::GetFontSize() / 16.0f, 0),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Guida rapida", p_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End(); return;
    }
    ImGui::TextWrapped("Benvenuto in MIXER.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "MIXER  un mixer audio software pensato per setup streaming: ti permette "
        "di decidere quali suoni vai ad ascoltare in cuffia e quali invece "
        "vanno catturati per lo streaming  separatamente.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Come si usa:");
    ImGui::Bullet(); ImGui::TextWrapped(
        "Ogni 'strip' (colonna a sinistra)  una sorgente: un microfono, "
        "o un VB-CABLE su cui hai instradato un'app (gioco, Discord, Spotify).");
    ImGui::Bullet(); ImGui::TextWrapped(
        "Ogni 'bus' (colonna a destra)  un'uscita: le cuffie, oppure "
        "VB-CABLE D che fa da 'mix per lo stream' che OBS catturer.");
    ImGui::Bullet(); ImGui::TextWrapped(
        "Per ogni strip scegli il device sorgente nel men a tendina, "
        "poi clicca sui quadratini colorati sotto al fader per decidere "
        "su quali bus inviare quel suono.");
    ImGui::Bullet(); ImGui::TextWrapped(
        "Le linee colorate sotto al mixer mostrano graficamente i collegamenti "
        "attivi: ti permettono di vedere a colpo d'occhio dove va ogni suono.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Preset:");
    ImGui::Bullet(); ImGui::TextWrapped(
        "Quando hai configurato tutto come ti piace, usa File  Salva preset "
        "per memorizzarlo su file. Potrai ricaricarlo con File  Apri preset.");
    ImGui::Spacing();
    ImGui::TextDisabled("Puoi riaprire questa guida da 'Aiuto  Guida rapida'.");
    if (ImGui::Button("Ho capito, chiudi", ImVec2(180, 0)))
        *p_open = false;
    ImGui::End();
}

static void RenderAboutPanel(bool* p_open)
{
    if (!*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(440.0f * ImGui::GetFontSize() / 16.0f, 0),
                             ImGuiCond_Always);
    if (!ImGui::Begin("Informazioni", p_open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
    { ImGui::End(); return; }
    ImGui::Text("MIXER  versione %s", MIXER_VERSION);
    ImGui::Separator();
    ImGui::TextWrapped("Mixer audio software per Windows basato su WASAPI shared mode.");
    ImGui::TextWrapped("UI: Dear ImGui  Build: C++17 + Visual Studio 2026.");
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(80, 0))) *p_open = false;
    ImGui::End();
}

// ── Aggiornamenti automatici ────────────────────────────────────────────────
// Stato UI del flusso di update (vive nello stack di WinMain).
struct UpdateUiState {
    bool             dismissed = false;   // l'utente ha scelto "Piu' tardi"
    bool             downloading = false; // download installer in corso
    std::atomic<int> result{0};           // 0=in corso, 1=lanciato, -1=errore
    std::thread      worker;              // thread di download non bloccante
    std::string      error;
};

// Avvia (o riavvia) il thread che scarica ed esegue l'installer.
static void StartUpdateDownload(mixer::update::Updater& updater, UpdateUiState& us)
{
    if (us.worker.joinable()) us.worker.join();
    us.downloading = true;
    us.result.store(0);
    us.worker = std::thread([&updater, &us]() {
        const bool ok = updater.downloadAndRun();
        if (!ok) us.error = updater.error();
        us.result.store(ok ? 1 : -1);
    });
}

// Modal di conferma aggiornamento. request_exit viene messo a true quando
// l'installer e' stato lanciato (l'app deve chiudersi per farsi sostituire).
static void RenderUpdateModal(mixer::update::Updater& updater,
                              UpdateUiState& us, bool& request_exit)
{
    using St = mixer::update::State;
    if (updater.state() != St::Available || us.dismissed) return;

    static const char* kPopup = "Aggiornamento disponibile##upd";
    if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const auto& nfo = updater.info();
    ImGui::Text("E' disponibile MIXER %s.", nfo.latest_version.c_str());
    ImGui::TextDisabled("Versione installata: %s", MIXER_VERSION);
    ImGui::Separator();
    ImGui::Spacing();

    const int res = us.result.load();

    if (us.downloading && res == 0) {
        ImGui::TextUnformatted("Scaricamento dell'installer in corso...");
        ImGui::TextDisabled("Al termine l'app si chiudera' per installare "
                            "l'aggiornamento.");
    } else if (res == -1) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Errore: %s", us.error.c_str());
        if (ImGui::Button("Riprova", ImVec2(140, 0)))
            StartUpdateDownload(updater, us);
        ImGui::SameLine();
        if (ImGui::Button("Chiudi", ImVec2(140, 0))) {
            us.dismissed = true;
            ImGui::CloseCurrentPopup();
        }
    } else {
        if (ImGui::Button("Aggiorna ora", ImVec2(140, 0)))
            StartUpdateDownload(updater, us);
        ImGui::SameLine();
        if (ImGui::Button("Piu' tardi", ImVec2(140, 0))) {
            us.dismissed = true;
            ImGui::CloseCurrentPopup();
        }
    }

    // Installer lanciato con successo: chiudi l'app.
    if (res == 1) {
        if (us.worker.joinable()) us.worker.join();
        request_exit = true;
    }

    ImGui::EndPopup();
}

// (RenderTestTonePanel rimosso: tutti i controlli del tono sono ora nella
//  toolbar di RenderMixerPanel  sempre visibili senza popup.)

// Render della sezione "Routing app" INLINE (no ImGui::Begin/End wrappers).
// Va chiamata dentro un'altra finestra (es. il pannello mixer).
static void RenderAppRoutingInline(const mixer::audio::DeviceList& devs,
                                   AppRoutingState& state)
{
    const float fs = ImGui::GetFontSize();

    // Combo "Uscita predefinita di sistema" (cambia il default Windows).
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Uscita predefinita di sistema:");
        ImGui::SameLine();

        std::wstring default_id;
        std::string  default_label = "<nessuna>";
        for (const auto& d : devs.render)
        {
            if (d.is_default)
            {
                default_id = d.id;
                default_label = shortDeviceName(d.friendly_name);
                break;
            }
        }
        ImGui::SetNextItemWidth(fs * 22.0f);
        if (ImGui::BeginCombo("##sysdef", default_label.c_str()))
        {
            for (const auto& d : devs.render)
            {
                std::string lbl = shortDeviceName(d.friendly_name);
                const bool sel = (d.id == default_id);
                if (ImGui::Selectable(lbl.c_str(), sel))
                {
                    if (mixer::audio::policy::setSystemDefaultEndpointAllRoles(d.id))
                        state.device_refresh_needed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cambia il device audio predefinito di Windows.\n"
                              "Tutte le app che non hanno un override per-app\n"
                              "useranno questo device.");

        ImGui::SameLine(0, fs * 1.5f);
        if (ImGui::Button("Apri Mixer Volume Windows"))
        {
            ::ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume",
                            nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Apre Impostazioni  Sistema  Audio  Mixer volume.\n"
                              "Da l puoi cambiare l'uscita di una SINGOLA app\n"
                              "(per-app override).");
    }
}

static void RenderDevicePanel(bool* p_open, const mixer::audio::DeviceList& devs)
{
    if (!*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(760.0f * ImGui::GetFontSize() / 16.0f,
                                    500.0f * ImGui::GetFontSize() / 16.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lista device audio", p_open))
    { ImGui::End(); return; }
    ImGui::TextDisabled("Premi F5 per aggiornare la lista.");

    auto render_row = [](const mixer::audio::AudioDevice& d) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (d.is_default) ImGui::TextUnformatted("");
        ImGui::TableSetColumnIndex(1);
        const std::string short_n = shortDeviceName(d.friendly_name);
        const std::string full_n  = narrow(d.friendly_name);
        ImGui::TextUnformatted(short_n.c_str());
        if (short_n != full_n && ImGui::IsItemHovered())
            ImGui::SetTooltip("Nome Windows originale:\n%s", full_n.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u Hz", d.sample_rate);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", (unsigned)d.channels);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("0x%08X", d.channel_mask);
    };
    const ImGuiTableFlags tflags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;

    if (ImGui::CollapsingHeader("Uscite (riproduzione)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##render", 5, tflags))
        {
            ImGui::TableSetupColumn("def", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("Nome");
            ImGui::TableSetupColumn("Sample rate", ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableSetupColumn("Canali",      ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Layout",      ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableHeadersRow();
            for (const auto& d : devs.render) render_row(d);
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Ingressi (microfoni e simili)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##capture", 5, tflags))
        {
            ImGui::TableSetupColumn("def", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("Nome");
            ImGui::TableSetupColumn("Sample rate", ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableSetupColumn("Canali",      ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Layout",      ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableHeadersRow();
            for (const auto& d : devs.capture) render_row(d);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// ---- Mixer panel with config + routing visualization -----------------------

namespace {

// Disegna un meter peak verticale con zone colorate
//   verde   da -inf a -12 dBFS
//   giallo  da -12 a -3 dBFS
//   rosso   da -3 a +6 dBFS
// peak_lin: ampiezza picco lineare (0..1+, 1.0 = 0 dBFS).
void drawMeterBar(ImVec2 p0, ImVec2 p1, float peak_lin)
{
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    if (w <= 0.0f || h <= 0.0f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Sfondo
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 20, 255));

    constexpr float db_min =  -60.0f;
    constexpr float db_max =   +6.0f;
    auto db_to_y = [&](float db) {
        const float t = (db - db_min) / (db_max - db_min);
        return p1.y - h * t;
    };
    const float y0db   = db_to_y(0.0f);
    const float ym3db  = db_to_y(-3.0f);
    const float ym12db = db_to_y(-12.0f);

    const float db = 20.0f * std::log10(std::max(peak_lin, 1e-6f));
    const float t  = std::clamp((db - db_min) / (db_max - db_min), 0.0f, 1.0f);

    if (t > 0.0f)
    {
        const float y_peak = p1.y - h * t;

        // Verde: dal fondo fino a min(yPeak, ym12db).
        const float y_green_top = std::max(y_peak, ym12db);
        dl->AddRectFilled(ImVec2(p0.x, y_green_top), ImVec2(p1.x, p1.y),
                          IM_COL32(60, 200, 80, 255));

        // Giallo: tra ym12db e ym3db (se peak supera -12).
        if (db > -12.0f)
        {
            const float y_yellow_top = std::max(y_peak, ym3db);
            dl->AddRectFilled(ImVec2(p0.x, y_yellow_top),
                              ImVec2(p1.x, ym12db),
                              IM_COL32(230, 210, 60, 255));
        }
        // Rosso: tra ym3db e yPeak (se peak supera -3).
        if (db > -3.0f)
        {
            dl->AddRectFilled(ImVec2(p0.x, y_peak), ImVec2(p1.x, ym3db),
                              IM_COL32(230, 80, 60, 255));
        }
    }

    // Tick a 0 dB (linea bianca tenue) e -12 dB.
    dl->AddLine(ImVec2(p0.x, y0db), ImVec2(p1.x, y0db),
                IM_COL32(255, 255, 255, 130), 1.0f);
    dl->AddLine(ImVec2(p0.x, ym12db), ImVec2(p1.x, ym12db),
                IM_COL32(255, 255, 255, 60), 1.0f);

    // Bordo
    dl->AddRect(p0, p1, IM_COL32(80, 85, 95, 255));
}

// Formatta un'ampiezza lineare in stringa dB. Sotto -60 dB mostra "-inf".
inline void formatDb(char* buf, size_t n, float lin)
{
    const float db = 20.0f * std::log10(std::max(lin, 1e-9f));
    if (db <= -60.0f) std::snprintf(buf, n, "-inf dB");
    else              std::snprintf(buf, n, "%+.1f dB", db);
}

// "Casella" read-only con valore numerico allineato a destra (aspetto da
// display). id rende univoco il widget; clickable abilita hover/click (usata
// per azzerare il picco). Ritorna true solo se clickable ed e' stata cliccata.
inline bool valueBox(const char* id, const char* text, float width,
                     ImU32 text_col, bool clickable)
{
    const ImU32 frame = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImGui::PushStyleColor(ImGuiCol_Button, frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        clickable ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        clickable ? ImGui::GetColorU32(ImGuiCol_FrameBgActive) : frame);
    if (text_col) ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));

    char lbl[64];
    std::snprintf(lbl, sizeof(lbl), "%s##%s", text, id);
    const bool clicked = ImGui::Button(lbl, ImVec2(width, 0.0f));

    ImGui::PopStyleVar();
    if (text_col) ImGui::PopStyleColor();
    ImGui::PopStyleColor(3);
    return clicked && clickable;
}

// Calcola decay factor per il delta-time corrente (fall ~25 dB/s).
inline float meterDecayFactor()
{
    const float dt = ImGui::GetIO().DeltaTime;
    return std::pow(10.0f, -25.0f * dt / 20.0f);
}

// Disegna un piccolo bottone toggle colorato per il routing strip-to-bus.
// Usa ImGui::Button (widget standard) con stili push per garantire hit-test
// affidabile su qualsiasi DPI/posizione (le versioni precedenti basate su
// InvisibleButton + AddRect avevano problemi di click in alcune posizioni).
bool routeButton(const char* id, ImU32 color_on, bool active, float side)
{
    ImGui::PushID(id);

    const ImVec4 on   = ImGui::ColorConvertU32ToFloat4(color_on);
    const ImVec4 off  = ImVec4(0.20f, 0.21f, 0.23f, 1.0f);
    const ImVec4 base = active ? on : off;
    const ImVec4 hov  = ImVec4(std::min(base.x * 1.25f, 1.0f),
                               std::min(base.y * 1.25f, 1.0f),
                               std::min(base.z * 1.25f, 1.0f), 1.0f);
    const ImVec4 act  = ImVec4(base.x * 0.80f, base.y * 0.80f,
                               base.z * 0.80f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Button,        base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
    ImGui::PushStyleColor(ImGuiCol_Border,
        active ? ImVec4(1, 1, 1, 0.55f) : ImVec4(0.4f, 0.42f, 0.45f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);

    // Label vuota ma con ID univoco via "##rb" + PushID
    const bool clicked = ImGui::Button("##rb", ImVec2(side, side));

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return clicked;
}

// Restituisce ASCII abbreviato di un'etichetta bus (es. "Cuf" da "Cuffie").
std::string busShort(const std::string& label)
{
    if (label.empty()) return "?";
    std::string out;
    for (char c : label)
    {
        if (out.size() >= 3) break;
        if ((unsigned char)c >= 0x80) continue; // skippa non-ASCII per evitare glyph mancanti
        out.push_back(c);
    }
    if (out.empty()) out = "B";
    return out;
}

} // namespace

static void RenderMixerPanel(bool* p_open,
                             mixer::config::MixerCfg& cfg,
                             const mixer::audio::DeviceList& devs,
                             UiState& ui,
                             TestToneState& tone,
                             mixer::audio::WasapiRenderStream& test_stream,
                             std::wstring& test_tone_device_id,
                             mixer::audio::AudioEngine& engine,
                             MeterDisplayState& meters,
                             AppRoutingState& app_routing)
{
    if (!*p_open) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float status_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - status_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##mixerroot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    const float fs = ImGui::GetFontSize();

    // --- TOOLBAR (sopra il mixer) ---------------------------------------
    // Riga 1: Avvia/Ferma motore mixer + stato.
    {
        const bool eng_running = engine.running();
        ImVec4 btn_color = eng_running
            ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f)   // rosso = stop
            : ImVec4(0.20f, 0.65f, 0.30f, 1.0f);  // verde = start
        ImGui::PushStyleColor(ImGuiCol_Button,        btn_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(btn_color.x * 1.15f, btn_color.y * 1.15f,
                   btn_color.z * 1.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(btn_color.x * 0.85f, btn_color.y * 0.85f,
                   btn_color.z * 0.85f, 1.0f));
        const char* lbl = eng_running ? "Ferma mixer" : "Avvia mixer";
        if (ImGui::Button(lbl, ImVec2(fs * 9.0f, fs * 2.0f)))
        {
            if (eng_running)
            {
                engine.stop();
                setStatus(ui, "Motore mixer fermato.");
            }
            else
            {
                if (engine.start(cfg, ui.buffer_ms, ui.exclusive_mode, ui.lowlat_mode))
                {
                    auto st = engine.getStatus();
                    char m[160];
                    std::snprintf(m, sizeof(m),
                                  "Motore mixer avviato: %zu capture, %zu render.",
                                  st.captures_opened, st.renders_opened);
                    setStatus(ui, m);
                }
                else
                {
                    setStatus(ui,
                        "Errore avvio motore: verifica che almeno una strip "
                        "abbia un device e sia routata su un bus con device.");
                }
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, fs * 1.0f);
        if (eng_running)
        {
            auto st = engine.getStatus();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "in funzione  ");
            ImGui::SameLine();
            ImGui::TextDisabled("%zu capture / %zu render",
                                st.captures_opened, st.renders_opened);
            if (st.last_error != 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                    " | warning: %s", hresultMessage(st.last_error));
            }
        }
        else
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("motore fermo  (configura strip e bus, poi premi 'Avvia mixer')");
            const long le = engine.getStatus().last_error;
            if (le != 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "  Ultimo errore: %s", hresultMessage(le));
            }
        }

    }

    // ----- Preset corrente + gestione del preset predefinito -----
    {
        const std::string pname =
            "Preset: " + narrow(presetDisplayName(ui.current_preset_path));
        ImGui::TextUnformatted(pname.c_str());

        // La spunta verde sta DENTRO il pulsante (a destra del testo) quando il
        // preset corrente E' il predefinito; cambiando preset sparisce.
        const bool is_default = (ui.current_preset_path == ui.default_preset_path);
        const float chk = ImGui::GetFontSize();
        const char* set_lbl = "Imposta come Predefinito";
        const float set_w = ImGui::CalcTextSize(set_lbl).x
                          + ImGui::GetStyle().FramePadding.x * 2.0f + chk * 1.6f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        const bool clicked_set = ImGui::Button(set_lbl, ImVec2(set_w, 0));
        ImGui::PopStyleVar();
        const ImVec2 set_min = ImGui::GetItemRectMin();
        const ImVec2 set_max = ImGui::GetItemRectMax();

        if (clicked_set)
        {
            ui.default_preset_path = ui.current_preset_path;
            saveDefaultPresetSetting(ui.default_preset_path);
            setStatus(ui, "Preset predefinito impostato: si carichera' all'avvio.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Carica questo preset automaticamente al prossimo avvio dell'app.");

        if (is_default)
            drawGreenCheckAt(ImVec2(set_max.x - chk * 1.3f,
                                    (set_min.y + set_max.y) * 0.5f - chk * 0.5f), chk);

        ImGui::SameLine(0, fs * 1.0f);
        if (ImGui::Button("Ripristina Preset di Default"))
        {
            cfg = mixer::config::makeDefaultConfig();
            cfg.normalize();
            ui.current_preset_path.clear();
            ui.default_preset_path.clear();
            saveDefaultPresetSetting(L"");
            setStatus(ui, "Preset vergine ripristinato e impostato come predefinito.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Torna al preset vergine e lo imposta come predefinito all'avvio.");
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Riga 2: Buffer audio, parametri test tono e bottone start/stop in una riga.
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Buffer audio:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 5.5f);
        if (ImGui::InputInt("ms##bufms", &ui.buffer_ms, 1, 5))
            ui.buffer_ms = std::clamp(ui.buffer_ms, 3, 200);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Dimensione del buffer del device audio in millisecondi.\n"
                              "Pi piccolo = pi reattivo ma rischio click.\n"
                              "Pi grande = pi stabile ma latenza maggiore.\n"
                              "Si applica al prossimo Start.\n"
                              "Range valido: 3..200 ms. Default: 10.");

        // Rileva la modalit BYPASS: una strip cattura in loopback un device
        // che  ANCHE assegnato a un bus. Significa che il gioco scrive
        // DIRETTAMENTE su quell'HDMI (Elgato) e MIXER lo loopback-cattura solo
        // per mandarlo in cuffia. In bypass l'audio per la registrazione NON
        // passa da MIXER  latenza Elgato fissa ~27 ms, indipendente dal buffer.
        bool bypass_mode = false;
        for (const auto& s : cfg.strips)
        {
            if (!s.loopback || s.device_id.empty()) continue;
            for (const auto& b : cfg.buses)
                if (!b.device_id.empty() && b.device_id == s.device_id)
                { bypass_mode = true; break; }
            if (bypass_mode) break;
        }

        // Periodi effettivi per modalit:
        //  - SHARED normale : arrotonda a ~10 ms (floor motore Win11)
        //  - EXCLUSIVE      : render ~3 ms, MA loopback resta shared ~10 ms
        //                     (WASAPI vieta loopback exclusive)
        //  - IAC3 lowlat    : SIA capture (anche loopback) SIA render ~3-4 ms
        const int eff_buffer = ui.buffer_ms < 10 ? 10 : ui.buffer_ms; // shared floor
        const int eff_iac3   = ui.buffer_ms < 3 ? 3 : ui.buffer_ms;   // IAC3 floor ~3
        // capture period: IAC3 lo abbassa anche col loopback; exclusive no.
        const int cap_period = ui.lowlat_mode ? eff_iac3 : eff_buffer;
        // render period: lo abbassano sia exclusive sia IAC3.
        const int ren_period = (ui.exclusive_mode || ui.lowlat_mode)
            ? eff_iac3 : eff_buffer;

        // Cuffie: 10 (Win engine sorgente) + cap_period + ren_period + 5 (USB).
        // Caso shared puro = 15 + 2*eff_buffer (equivalente).
        const int lat_razer = (ui.exclusive_mode || ui.lowlat_mode)
            ? (10 + cap_period + ren_period + 5)
            : (15 + 2 * eff_buffer);
        // Elgato/OBS:
        //   bypass    = Game->HDMI->Elgato->OBS, niente MIXER  ~27 ms FISSO
        //               (n exclusive n IAC3 lo toccano: non passa MIXER)
        //   via MIXER = 13 (HDMI+Elgato+OBS) + cap_period + ren_period
        const int lat_elgato = bypass_mode
            ? 27
            : ((ui.exclusive_mode || ui.lowlat_mode)
                ? (13 + cap_period + ren_period)
                : (28 + 2 * eff_buffer));

        const char* mode_tag = ui.exclusive_mode ? " EXCL"
                              : (ui.lowlat_mode  ? " IAC3" : "");
        ImGui::SameLine(0, fs * 1.0f);
        ImGui::TextDisabled("(stima end-to-end%s:", mode_tag);
        ImGui::SameLine(0, fs * 0.4f);
        ImGui::TextColored(ImVec4(0.7f, 0.95f, 0.7f, 1.0f),
                           "Cuffie ~%d ms", lat_razer);
        ImGui::SameLine(0, fs * 0.4f);
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.7f, 1.0f),
                           "/  Elgato ~%d ms%s", lat_elgato,
                           bypass_mode ? " (bypass)" : "");
        ImGui::SameLine(0, fs * 0.2f);
        ImGui::TextDisabled(")");
        if (ImGui::IsItemHovered())
        {
            if (bypass_mode)
                ImGui::SetTooltip(
                    "Modalit BYPASS rilevata (preset Low Latency).\n"
                    "Modo attivo: %s\n"
                    "Cuffie : Game  HDMI  MIXER loopback  Razer\n"
                    "         period capture %d ms + render %d ms\n"
                    "Elgato : Game  HDMI  Elgato  OBS  (NON passa MIXER)\n"
                    "         = ~27 ms FISSO (nessun modo lo cambia)\n"
                    "\n"
                    "Buffer richiesto: %d ms\n"
                    "  SHARED  : arrotonda a ~10 ms (capture+render)\n"
                    "  EXCLUSIVE: render ~3 ms, loopback resta ~10 ms\n"
                    "  IAC3     : capture E render ~3 ms (anche loopback)\n"
                    "\n"
                    "Per sincronizzare A/V in OBS aggiungi ~%d ms\n"
                    "di ritardo al video.",
                    (ui.exclusive_mode ? "EXCLUSIVE"
                        : (ui.lowlat_mode ? "IAC3 low-latency"
                                          : "SHARED normale")),
                    cap_period, ren_period, ui.buffer_ms, 27 - 15);
            else
                ImGui::SetTooltip(
                    "Routing attraverso MIXER.\n"
                    "Modo attivo: %s\n"
                    "Cuffie : Game  VB-CABLE  MIXER  Razer\n"
                    "Elgato : Game  VB-CABLE  MIXER  HDMI  Elgato  OBS\n"
                    "\n"
                    "Buffer richiesto: %d ms\n"
                    "period capture %d ms + render %d ms\n"
                    "\n"
                    "Per sincronizzare A/V in OBS aggiungi ~%d ms al video.",
                    (ui.exclusive_mode ? "EXCLUSIVE"
                        : (ui.lowlat_mode ? "IAC3 low-latency"
                                          : "SHARED normale")),
                    ui.buffer_ms, cap_period, ren_period, lat_elgato - 25);
        }

        // Opzione Exclusive mode, subito dopo le statistiche di latenza.
        ImGui::SameLine(0, fs * 1.5f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, fs * 1.0f);

        const bool eng_on = engine.running();
        auto restartEngine = [&](const char* msg) {
            if (eng_on)
            {
                engine.stop();
                engine.start(cfg, ui.buffer_ms,
                             ui.exclusive_mode, ui.lowlat_mode);
                setStatus(ui, msg, 5.0);
            }
        };

        if (ImGui::Checkbox("Exclusive mode", &ui.exclusive_mode))
        {
            if (ui.exclusive_mode) ui.lowlat_mode = false; // mutua esclusione
            restartEngine(ui.exclusive_mode
                ? "Exclusive mode ON  motore riavviato."
                : "Exclusive mode OFF  motore riavviato.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "WASAPI Exclusive mode: MIXER prende il controllo ESCLUSIVO\n"
                "dei device (Razer, HDMI). Periodo ~3-5 ms.\n"
                "\n"
                "Limiti:\n"
                " - Nessun'altra app pu usare quei device mentre MIXER  on\n"
                " - Loopback resta shared (WASAPI vieta loopback exclusive),\n"
                "   quindi in bypass il guadagno  solo sul render (Razer).\n"
                " - Fallback automatico a shared se non supportato.\n"
                "\n"
                "Mutuamente esclusivo con Low-Latency (IAC3).");

        ImGui::SameLine(0, fs * 1.0f);
        if (ImGui::Checkbox("Low-Latency (IAC3)", &ui.lowlat_mode))
        {
            if (ui.lowlat_mode) ui.exclusive_mode = false; // mutua esclusione
            restartEngine(ui.lowlat_mode
                ? "Low-Latency IAC3 ON  motore riavviato."
                : "Low-Latency IAC3 OFF  motore riavviato.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "IAudioClient3 low-latency SHARED: periodo motore ~3 ms\n"
                "invece dei ~10 ms standard, MA il device resta CONDIVISO\n"
                "(altre app possono usarlo) e funziona ANCHE col loopback.\n"
                "\n"
                "Per il preset bypass  la scelta migliore: abbassa sia la\n"
                "cattura loopback sia il render (Exclusive non pu toccare\n"
                "il loopback). Cuffie stimate ~20-22 ms.\n"
                "\n"
                "Limiti:\n"
                " - Richiede che il driver del device dichiari un periodo\n"
                "   minimo corto. Fallback automatico a shared normale se no.\n"
                " - Il buffer interno del gioco (~10-40 ms) resta comunque.\n"
                "\n"
                "Mutuamente esclusivo con Exclusive mode.");

        ImGui::SameLine(0, fs * 1.5f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, fs * 1.0f);

        ImGui::Checkbox("Abilita dBFS", &ui.enable_dbfs);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ON (default): i fader dB di strip/bus regolano il volume\n"
                              "e c' soft-clip a 0 dBFS per evitare distorsione su somme.\n\n"
                              "OFF: l'audio passa attraverso il mixer SENZA essere\n"
                              "modificato. I fader vengono ignorati. Mute/Solo per\n"
                              "funzionano sempre.");

        ImGui::SameLine(0, fs * 1.5f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, fs * 1.0f);

        if (ImGui::Button("Aggiorna device", ImVec2(fs * 9.0f, 0)))
            app_routing.device_refresh_needed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rilegge la lista dei device audio da Windows.\n"
                              "Premi qui se hai collegato/scollegato uno speaker,\n"
                              "una cuffia, l'Elgato o cambiato configurazione audio\n"
                              "mentre il MIXER era aperto.");
    }

    ImGui::Separator();
    ImGui::Spacing();

    // --- Routing app inline (sopra al mixer) ---
    if (ImGui::CollapsingHeader("Audio di sistema",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        RenderAppRoutingInline(devs, app_routing);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Mixer principale");
    ImGui::Spacing();

    cfg.normalize();
    meters.resize(cfg.strips.size(), cfg.buses.size());

    const float strip_w  = fs * 9.5f;
    const float gap_x    = fs * 0.5f;
    const float route_btn_side = fs * 1.4f;
    // La sezione "invia a:" ora ha una riga (toggle + manopola %) per bus,
    // quindi l'altezza della strip cresce col numero di bus per evitare
    // scrollbar interne. Base = tutto il resto della strip (fader, dB, ecc.).
    const float route_row_h = route_btn_side + fs * 0.35f;
    const size_t n_route_rows = cfg.buses.empty() ? 1 : cfg.buses.size();
    const float strip_h  = fs * 20.0f + (float)n_route_rows * route_row_h;
    const float meter_w  = fs * 0.55f;
    const float meter_gap = 2.0f;
    const float decay    = meterDecayFactor();

    // Indici per cancellazione differita (la cancellazione avviene DOPO il
    // loop di disegno, per non invalidare l'iterazione corrente).
    int strip_to_delete = -1;
    int bus_to_delete   = -1;

    std::vector<float> strip_center_x(cfg.strips.size(), 0.0f);
    std::vector<float> bus_center_x  (cfg.buses.size(),  0.0f);
    float row_bottom_y = 0.0f;

    const float box_w = fs * 4.2f;  // larghezza caselle livello/picco

    // Disegna le due caselle accanto al meter: livello attuale (zona-colorato)
    // e picco massimo raggiunto (click = azzera). hold_lin viene aggiornato a 0
    // se l'utente clicca la casella del picco.
    auto drawLevelBoxes = [&](float level_lin, float& hold_lin)
    {
        char buf[32];
        // --- livello attuale (tempo reale) ---
        const float db_now = 20.0f * std::log10(std::max(level_lin, 1e-9f));
        const ImU32 col = db_now >= -3.0f  ? IM_COL32(235,  80,  80, 255)
                        : db_now >= -12.0f ? IM_COL32(230, 200,  70, 255)
                                           : IM_COL32(120, 210, 120, 255);
        formatDb(buf, sizeof(buf), level_lin);
        valueBox("rt", buf, box_w, col, /*clickable*/ false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Livello attuale in dB (tempo reale).");

        // --- picco massimo raggiunto ---
        formatDb(buf, sizeof(buf), hold_lin);
        const bool reset = valueBox("pk", buf, box_w,
                                    IM_COL32(210, 212, 220, 255), /*clickable*/ true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Picco massimo raggiunto.\nClick per azzerare.");
        if (reset) hold_lin = 0.0f;
    };

    auto drawStrip = [&](int i)
    {
        auto& s = cfg.strips[i];
        ImGui::PushID(100 + i);
        ImGui::BeginChild(ImGui::GetID("strip"), ImVec2(strip_w, strip_h), true);

        // Label editabile + bottone X (elimina strip) sulla stessa riga.
        {
            const float x_btn_w = fs * 1.6f;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", s.label.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
                                    - x_btn_w - fs * 0.3f);
            if (ImGui::InputText("##lbl", buf, sizeof(buf)))
                s.label = buf;

            ImGui::SameLine(0, fs * 0.2f);
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(110, 35, 35, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(160, 50, 50, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200, 70, 70, 255));
            if (ImGui::Button("X##del", ImVec2(x_btn_w, 0)))
                strip_to_delete = i;
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Elimina questa strip.\n"
                                  "Se il motore  in funzione, ferma e riavvia\n"
                                  "per applicare la nuova topologia.");
        }

        ImGui::Separator();

        // Device combo
        bool dummy_changed = deviceComboBox("##dev", devs,
                                            /*capture*/ true,
                                            /*loopback*/ true,
                                            /*render*/ false,
                                            &s.device_id, &s.loopback);
        (void)dummy_changed;

        if (s.device_id.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Nessun device scelto");
        else if (s.loopback)
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "loopback");
        else
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "ingresso");

        // Fader verticale (con reset su click destro)
        const float fader_h = strip_h
                              - ImGui::GetTextLineHeightWithSpacing() * 10.0f;
        ImGui::VSliderFloat("##g", ImVec2(fs * 1.6f, fader_h),
                            &s.gain_db, -60.0f, 12.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Trascina per regolare il livello in dB.\n"
                              "Click destro o doppio click: reset a 0 dB.");
        if (ImGui::IsItemHovered()
            && (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
            s.gain_db = 0.0f;

        // Meter L/R accanto al fader, stessa altezza.
        {
            const float fresh_L = engine.readStripPeak((size_t)i, 0);
            const float fresh_R = engine.readStripPeak((size_t)i, 1);
            auto& mdL = meters.strip[(size_t)i][0];
            auto& mdR = meters.strip[(size_t)i][1];
            mdL = std::max(mdL * decay, fresh_L);
            mdR = std::max(mdR * decay, fresh_R);
            // Picco massimo raggiunto (non decade: hold fino a reset utente).
            float& hold = meters.strip_hold[(size_t)i];
            hold = std::max(hold, std::max(fresh_L, fresh_R));

            ImGui::SameLine(0, meter_gap);
            ImVec2 m0 = ImGui::GetCursorScreenPos();
            drawMeterBar(m0, ImVec2(m0.x + meter_w, m0.y + fader_h), mdL);
            ImGui::Dummy(ImVec2(meter_w, fader_h));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Peak meter canale L (post-fader)\n%.1f dBFS",
                                  20.0f * std::log10(std::max(mdL, 1e-6f)));

            ImGui::SameLine(0, 1.0f);
            m0 = ImGui::GetCursorScreenPos();
            drawMeterBar(m0, ImVec2(m0.x + meter_w, m0.y + fader_h), mdR);
            ImGui::Dummy(ImVec2(meter_w, fader_h));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Peak meter canale R (post-fader)\n%.1f dBFS",
                                  20.0f * std::log10(std::max(mdR, 1e-6f)));
        }

        // Accanto al meter: casella livello attuale + casella picco massimo,
        // poi i tasti Mute/Solo sotto.
        ImGui::SameLine();
        ImGui::BeginGroup();
        drawLevelBoxes(std::max(meters.strip[(size_t)i][0],
                                meters.strip[(size_t)i][1]),
                       meters.strip_hold[(size_t)i]);
        ImGui::Spacing();
        ImGui::Checkbox("M", &s.mute);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute: silenzia questa sorgente.");
        ImGui::SameLine();
        ImGui::Checkbox("S", &s.solo);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: ascolta solo le strip in solo.");
        ImGui::EndGroup();

        // Input numerico dB editabile direttamente
        ImGui::SetNextItemWidth(fs * 6.5f);
        if (ImGui::InputFloat("##gnum", &s.gain_db, 0.5f, 3.0f, "%.1f dB"))
            s.gain_db = std::clamp(s.gain_db, -60.0f, 12.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click per scrivere il valore in dB.\n"
                              "Click destro per reset a 0 dB.");
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            s.gain_db = 0.0f;

        // Route: per ogni bus un toggle on/off + manopola volume di invio
        // (1..100%). Cos la stessa strip pu andare al Bus 1 al 50% e al
        // Bus 2 al 100%. La manopola  disabilitata se la rotta  spenta.
        ImGui::Separator();
        ImGui::TextDisabled("invia a:");
        if (s.route_level.size() < cfg.buses.size())
            s.route_level.resize(cfg.buses.size(), 1.0f);
        for (size_t b = 0; b < cfg.buses.size(); ++b)
        {
            const bool on = s.routes[b];
            char rid[16]; std::snprintf(rid, sizeof(rid), "r%zu", b);
            if (routeButton(rid, busColor((int)b), on, route_btn_side))
                s.routes[b] = !s.routes[b];
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s questa sorgente al bus '%s'",
                                  on ? "Non inviare pi" : "Invia",
                                  cfg.buses[b].label.c_str());
            }

            ImGui::SameLine(0, 5);
            int pct = std::clamp(
                (int)std::lround(s.route_level[b] * 100.0f), 1, 100);
            ImGui::BeginDisabled(!on);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            char sid[16]; std::snprintf(sid, sizeof(sid), "##rl%zu", b);
            if (ImGui::SliderInt(sid, &pct, 1, 100, "%d%%"))
                s.route_level[b] = std::clamp(pct, 1, 100) / 100.0f;
            ImGui::EndDisabled();
            if (on && ImGui::IsItemHovered())
                ImGui::SetTooltip("Volume di '%s' verso '%s': %d%%\n"
                                  "Trascina per regolare il livello di invio.",
                                  s.label.c_str(),
                                  cfg.buses[b].label.c_str(), pct);
        }

        ImGui::EndChild();
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        strip_center_x[i] = (mn.x + mx.x) * 0.5f;
        row_bottom_y = std::max(row_bottom_y, mx.y);
        ImGui::PopID();
    };

    auto drawBus = [&](int i)
    {
        auto& b = cfg.buses[i];
        ImGui::PushID(200 + i);
        ImGui::BeginChild(ImGui::GetID("bus"), ImVec2(strip_w, strip_h), true);

        // Indicatore di colore bus
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 col_p0 = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(col_p0,
                          ImVec2(col_p0.x + strip_w, col_p0.y + fs * 0.4f),
                          busColor(i), 2.0f);
        ImGui::Dummy(ImVec2(strip_w, fs * 0.4f));

        // Label editabile + X (elimina bus).
        {
            const float x_btn_w = fs * 1.6f;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", b.label.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
                                    - x_btn_w - fs * 0.3f);
            if (ImGui::InputText("##lbl", buf, sizeof(buf)))
                b.label = buf;

            ImGui::SameLine(0, fs * 0.2f);
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(110, 35, 35, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(160, 50, 50, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200, 70, 70, 255));
            if (ImGui::Button("X##del", ImVec2(x_btn_w, 0)))
                bus_to_delete = i;
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Elimina questo bus.\n"
                                  "Le route delle strip verso questo bus\n"
                                  "vengono rimosse automaticamente.\n"
                                  "Se il motore  in funzione, ferma e riavvia.");
        }

        ImGui::Separator();

        bool loop_dummy = false;
        deviceComboBox("##dev", devs,
                       /*capture*/  false,
                       /*loopback*/ false,
                       /*render*/   true,
                       &b.device_id, &loop_dummy);

        if (b.device_id.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Nessun device scelto");
        else
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "uscita");

        const float fader_h = strip_h
                              - ImGui::GetTextLineHeightWithSpacing() * 10.0f;
        ImGui::VSliderFloat("##g", ImVec2(fs * 1.6f, fader_h),
                            &b.gain_db, -60.0f, 12.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Trascina per regolare il livello in dB.\n"
                              "Click destro o doppio click: reset a 0 dB.");
        if (ImGui::IsItemHovered()
            && (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
            b.gain_db = 0.0f;

        // Meter L/R accanto al fader, stessa altezza.
        {
            const float fresh_L = engine.readBusPeak((size_t)i, 0);
            const float fresh_R = engine.readBusPeak((size_t)i, 1);
            auto& mdL = meters.bus[(size_t)i][0];
            auto& mdR = meters.bus[(size_t)i][1];
            mdL = std::max(mdL * decay, fresh_L);
            mdR = std::max(mdR * decay, fresh_R);
            // Picco massimo raggiunto (non decade: hold fino a reset utente).
            float& hold = meters.bus_hold[(size_t)i];
            hold = std::max(hold, std::max(fresh_L, fresh_R));

            ImGui::SameLine(0, meter_gap);
            ImVec2 m0 = ImGui::GetCursorScreenPos();
            drawMeterBar(m0, ImVec2(m0.x + meter_w, m0.y + fader_h), mdL);
            ImGui::Dummy(ImVec2(meter_w, fader_h));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Peak meter canale L (uscita bus)\n%.1f dBFS",
                                  20.0f * std::log10(std::max(mdL, 1e-6f)));

            ImGui::SameLine(0, 1.0f);
            m0 = ImGui::GetCursorScreenPos();
            drawMeterBar(m0, ImVec2(m0.x + meter_w, m0.y + fader_h), mdR);
            ImGui::Dummy(ImVec2(meter_w, fader_h));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Peak meter canale R (uscita bus)\n%.1f dBFS",
                                  20.0f * std::log10(std::max(mdR, 1e-6f)));
        }

        // Accanto al meter: casella livello attuale + casella picco massimo,
        // poi il tasto Mute sotto.
        ImGui::SameLine();
        ImGui::BeginGroup();
        drawLevelBoxes(std::max(meters.bus[(size_t)i][0],
                                meters.bus[(size_t)i][1]),
                       meters.bus_hold[(size_t)i]);
        ImGui::Spacing();
        ImGui::Checkbox("M", &b.mute);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute: silenzia questo bus.");
        ImGui::EndGroup();

        // Input numerico dB editabile direttamente
        ImGui::SetNextItemWidth(fs * 6.5f);
        if (ImGui::InputFloat("##gnum", &b.gain_db, 0.5f, 3.0f, "%.1f dB"))
            b.gain_db = std::clamp(b.gain_db, -60.0f, 12.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click per scrivere il valore in dB.\n"
                              "Click destro per reset a 0 dB.");
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            b.gain_db = 0.0f;

        ImGui::Separator();
        ImGui::TextDisabled("bus '%s'", busShort(b.label).c_str());

        ImGui::EndChild();
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        bus_center_x[i] = (mn.x + mx.x) * 0.5f;
        row_bottom_y = std::max(row_bottom_y, mx.y);
        ImGui::PopID();
    };

    // Area scorrevole: con molte strip/bus la riga supera la larghezza della
    // finestra. Questo child mostra una scrollbar ORIZZONTALE cos l'utente
    // pu spostarsi ai lati e raggiungere le sezioni fuori inquadratura.
    // NoScrollWithMouse: la rotella sull'area ESTERNA non deve scorrere in
    // verticale; la gestiamo noi -> scorrimento orizzontale (vedi sotto).
    // I child interni strip/bus restano default: dentro di loro la rotella
    // continua a scorrere su/gi il loro contenuto, com'era prima.
    ImGui::BeginChild("##mixerscroll", ImVec2(0, 0), 0,
                      ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    // Rotella = scorri a destra/sinistra SOLO fuori dalle strip/bus.
    // IsWindowHovered() senza ChildWindows  vero solo quando il puntatore
    //  sull'area esterna (spazi tra le sezioni, canvas di routing) e NON
    // sopra un child strip/bus: l, la rotella scorre il loro contenuto.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered() &&
            (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
        {
            const float step = strip_w + gap_x;   // ~una strip per tacca
            const float dx   = (io.MouseWheel + io.MouseWheelH) * step;
            ImGui::SetScrollX(ImGui::GetScrollX() - dx);
        }
    }

    // --- Riga strip + spacer + bus ---
    for (size_t i = 0; i < cfg.strips.size(); ++i)
    {
        drawStrip((int)i);
        ImGui::SameLine(0, gap_x);
    }

    // Bottone "+" per aggiungere una nuova strip (sempre dopo l'ultima).
    {
        const float plus_w = fs * 2.6f;
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(40, 60, 45, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 100, 70, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(80, 140, 90, 255));
        if (ImGui::Button("+##addstrip", ImVec2(plus_w, strip_h)))
        {
            mixer::config::StripCfg s;
            s.label = "Strip " + std::to_string(cfg.strips.size() + 1);
            cfg.strips.push_back(std::move(s));
            cfg.normalize();
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Aggiungi una nuova strip al mixer.\n"
                              "Se il motore  in funzione, ferma e riavvia\n"
                              "per applicare la nuova topologia.");
    }

    if (!cfg.buses.empty())
    {
        ImGui::SameLine(0, fs * 1.2f);
        // Divisore verticale
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(pos.x - fs * 0.6f, pos.y),
            ImVec2(pos.x - fs * 0.6f, pos.y + strip_h),
            IM_COL32(80, 82, 90, 255), 1.5f);
    }

    for (size_t i = 0; i < cfg.buses.size(); ++i)
    {
        drawBus((int)i);
        ImGui::SameLine(0, gap_x);
    }

    // Bottone "+" per aggiungere un nuovo bus.
    {
        const float plus_w = fs * 2.6f;
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(60, 45, 70, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 70, 110, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(120, 90, 150, 255));
        if (ImGui::Button("+##addbus", ImVec2(plus_w, strip_h)))
        {
            mixer::config::BusCfg b;
            b.label = "Bus " + std::to_string(cfg.buses.size() + 1);
            cfg.buses.push_back(std::move(b));
            cfg.normalize();
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Aggiungi un nuovo bus al mixer.\n"
                              "Se il motore  in funzione, ferma e riavvia\n"
                              "per applicare la nuova topologia.");
    }

    // Bordo destro della riga (ultimo elemento = bottone "+bus"): serve per
    // dimensionare il canvas di routing in modo che copra TUTTA la larghezza
    // scorrevole e non solo la parte visibile.
    const float row_right_x = ImGui::GetItemRectMax().x;

    // Applica cancellazioni differite (clic X sui pulsanti delle strip/bus).
    // Vengono fatte qui DOPO il disegno per non invalidare l'iterazione.
    if (bus_to_delete >= 0 && bus_to_delete < (int)cfg.buses.size())
    {
        cfg.buses.erase(cfg.buses.begin() + bus_to_delete);
        // Rimuove la voce di route corrispondente da OGNI strip, in modo
        // che la route del bus successivo non si sposti su un altro slot.
        for (auto& s : cfg.strips)
        {
            if ((size_t)bus_to_delete < s.routes.size())
                s.routes.erase(s.routes.begin() + bus_to_delete);
        }
        cfg.normalize();
    }
    if (strip_to_delete >= 0 && strip_to_delete < (int)cfg.strips.size())
    {
        cfg.strips.erase(cfg.strips.begin() + strip_to_delete);
        cfg.normalize();
    }

    // --- Routing canvas sotto la riga ---
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float lane_h    = fs * 1.7f;
    const float canvas_h  = std::max(fs * 4.0f, lane_h * (cfg.buses.size() + 0.5f));
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    // Larghezza canvas = almeno lo spazio visibile, ma estesa fino al bordo
    // destro della riga di strip/bus cos lo sfondo e le linee di routing
    // restano allineati mentre si scorre lateralmente.
    const float canvas_w = std::max(ImGui::GetContentRegionAvail().x,
                                    row_right_x - canvas_p0.x + fs * 0.5f);
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_w,
                              canvas_p0.y + canvas_h);

    ImGui::InvisibleButton("##routing_canvas",
                           ImVec2(canvas_w, canvas_h));

    dl->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(20, 22, 28, 255), 6.0f);
    dl->AddRect      (canvas_p0, canvas_p1, IM_COL32(60, 64, 72, 255), 6.0f, 0, 1.0f);

    // Etichetta in alto a sinistra
    dl->AddText(ImVec2(canvas_p0.x + 8, canvas_p0.y + 4),
                IM_COL32(160, 165, 175, 255),
                "Routing audio  (sorgente  bus)");

    // Linee verticali tenui per i centri delle strip e dei bus (riferimento)
    for (float x : strip_center_x)
        if (x > 0)
            dl->AddLine(ImVec2(x, canvas_p0.y), ImVec2(x, canvas_p1.y),
                        IM_COL32(40, 44, 50, 255), 1.0f);
    for (float x : bus_center_x)
        if (x > 0)
            dl->AddLine(ImVec2(x, canvas_p0.y), ImVec2(x, canvas_p1.y),
                        IM_COL32(40, 44, 50, 255), 1.0f);

    const float lane_top = canvas_p0.y + fs * 1.6f;

    // Per ogni bus, una lane: drop verticali dalle strip routate + rail + drop al bus.
    for (size_t b = 0; b < cfg.buses.size(); ++b)
    {
        const float y = lane_top + (float)b * lane_h;
        if (y > canvas_p1.y - 4.0f) break;

        const ImU32 col_strong = busColor((int)b);
        const ImU32 col_soft   = busColor((int)b, 0.35f);

        // Etichetta del bus a destra della rail
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s", cfg.buses[b].label.c_str());

        // Raccogli x delle strip routate verso questo bus `b`.
        std::vector<float> active_xs;
        for (size_t i = 0; i < cfg.strips.size(); ++i)
            if (b < cfg.strips[i].routes.size() && cfg.strips[i].routes[b])
                active_xs.push_back(strip_center_x[i]);

        if (active_xs.empty())
        {
            // Linea piatta tenue per indicare "bus vuoto"
            dl->AddLine(ImVec2(canvas_p0.x + 12, y),
                        ImVec2(bus_center_x[b], y),
                        col_soft, 1.5f);
            dl->AddCircleFilled(ImVec2(bus_center_x[b], y), 4.0f, col_soft);
            dl->AddText(ImVec2(canvas_p0.x + 16, y - fs * 0.65f),
                        col_soft, lbl);
            continue;
        }

        const float left_x  = *std::min_element(active_xs.begin(), active_xs.end());
        const float right_x = std::max(*std::max_element(active_xs.begin(), active_xs.end()),
                                       bus_center_x[b]);

        // Rail orizzontale
        dl->AddLine(ImVec2(left_x, y), ImVec2(right_x, y), col_strong, 3.0f);

        // Drop verticali dalle strip
        for (float x : active_xs)
        {
            dl->AddLine(ImVec2(x, canvas_p0.y + 2), ImVec2(x, y), col_strong, 2.0f);
            dl->AddCircleFilled(ImVec2(x, y), 4.5f, col_strong);
        }
        // Drop su al bus
        dl->AddLine(ImVec2(bus_center_x[b], y), ImVec2(bus_center_x[b], canvas_p0.y + 2),
                    col_strong, 2.5f);
        dl->AddCircleFilled(ImVec2(bus_center_x[b], y), 5.5f, col_strong);

        dl->AddText(ImVec2(right_x + 8, y - fs * 0.65f), col_strong, lbl);
    }

    ImGui::EndChild();   // chiude "##mixerscroll" (area scorrevole)

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---- Menu bar / status bar -------------------------------------------------

static void RenderMenuBar(UiState& ui,
                          mixer::config::MixerCfg& cfg,
                          mixer::audio::WasapiRenderStream& test_stream,
                          HWND hwnd,
                          bool& request_refresh,
                          bool& request_exit,
                          mixer::update::Updater& updater,
                          UpdateUiState& update_ui)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Nuovo preset"))
        {
            cfg = mixer::config::makeDefaultConfig();
            ui.current_preset_path.clear();
            setStatus(ui, "Preset reimpostato a quello iniziale.");
        }
        if (ImGui::MenuItem("Apri preset...", "Ctrl+O"))
        {
            std::wstring path = openPresetDialog(hwnd);
            if (!path.empty())
            {
                mixer::config::MixerCfg loaded;
                if (mixer::config::loadFromFile(path, loaded))
                {
                    cfg = std::move(loaded);
                    cfg.normalize();
                    ui.current_preset_path = path;
                    setStatus(ui, "Preset caricato: " + narrow(path));
                }
                else
                {
                    setStatus(ui, "Errore: impossibile leggere il file preset.");
                }
            }
        }
        const bool has_path = !ui.current_preset_path.empty();
        if (ImGui::MenuItem("Salva preset", "Ctrl+S", false, has_path))
        {
            if (mixer::config::saveToFile(ui.current_preset_path, cfg))
                setStatus(ui, "Preset salvato.");
            else
                setStatus(ui, "Errore nel salvataggio del preset.");
        }
        if (ImGui::MenuItem("Salva preset come...", "Ctrl+Shift+S"))
        {
            std::wstring path = savePresetDialog(hwnd, ui.current_preset_path);
            if (!path.empty())
            {
                if (mixer::config::saveToFile(path, cfg))
                {
                    ui.current_preset_path = path;
                    setStatus(ui, "Preset salvato: " + narrow(path));
                }
                else
                {
                    setStatus(ui, "Errore nel salvataggio del preset.");
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Aggiorna lista device", "F5"))
            request_refresh = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Esci", "Alt+F4"))
            request_exit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Vista"))
    {
        ImGui::MenuItem("Mixer principale", nullptr, &ui.show_mixer);
        ImGui::MenuItem("Lista device audio", nullptr, &ui.show_devices);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Aiuto"))
    {
        ImGui::MenuItem("Guida rapida", nullptr, &ui.show_welcome);
        ImGui::MenuItem("Informazioni", nullptr, &ui.show_about);
        ImGui::Separator();
        {
            using St = mixer::update::State;
            const St st = updater.state();
            const bool checking = (st == St::Checking);
            if (ImGui::MenuItem("Controlla aggiornamenti", nullptr, false, !checking)) {
                update_ui.dismissed = false;     // riapri il prompt se serve
                updater.checkAsync(MIXER_VERSION);
            }
            if (checking)
                ImGui::TextDisabled("  Controllo in corso...");
            else if (st == St::UpToDate)
                ImGui::TextDisabled("  Sei all'ultima versione.");
            else if (st == St::Error)
                ImGui::TextDisabled("  Controllo non riuscito.");
            else if (st == St::Available)
                ImGui::TextDisabled("  Aggiornamento disponibile!");
        }
        ImGui::EndMenu();
    }

    // Indicatore stato a destra
    const char* state_text = test_stream.running()
        ? "Audio: in riproduzione" : "Audio: fermo";
    ImVec4 color = test_stream.running()
        ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    const float text_w = ImGui::CalcTextSize(state_text).x;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - text_w - 16.0f);
    ImGui::TextColored(color, "%s", state_text);

    ImGui::EndMainMenuBar();
}

static void RenderStatusBar(const UiState& ui,
                            const mixer::audio::DeviceList& devs,
                            const mixer::config::MixerCfg& cfg,
                            mixer::audio::WasapiRenderStream& test_stream)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bar_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    if (ImGui::Begin("##statusbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav))
    {
        if (!ui.status_message.empty() && ImGui::GetTime() < ui.status_until_time)
        {
            ImGui::TextColored(ImVec4(0.8f, 0.95f, 1.0f, 1.0f), "%s",
                               ui.status_message.c_str());
        }
        else
        {
            ImGui::Text("Strip: %zu", cfg.strips.size());
            ImGui::SameLine(); ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            ImGui::Text("Bus: %zu", cfg.buses.size());
            ImGui::SameLine(); ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            ImGui::Text("Device render: %zu", devs.render.size());
            ImGui::SameLine(); ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            ImGui::Text("Device capture: %zu", devs.capture.size());
            ImGui::SameLine(); ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            if (test_stream.running())
            {
                ImGui::Text("Test tono: %u Hz, %u ch",
                            test_stream.sampleRate(), test_stream.channels());
                ImGui::SameLine(); ImGui::TextDisabled(" | ");
                ImGui::SameLine();
            }
            ImGui::TextDisabled("F5 = aggiorna device");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---- Smoke test ------------------------------------------------------------

static int runSmokeTest()
{
    FILE* f = nullptr;
    fopen_s(&f, "smoke.log", "wb");
    if (!f) return 100;
    auto log = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fflush(f);
    };

    log("== MIXER smoke test ==");
    int failures = 0;

    log("[1] Device enumeration");
    auto devs = mixer::audio::enumerateAllDevices();
    log("    render=%zu capture=%zu", devs.render.size(), devs.capture.size());
    log("");
    log("    --- RENDER (uscite/riproduzione) ---");
    for (size_t i = 0; i < devs.render.size(); ++i)
    {
        const auto& d = devs.render[i];
        log("    [r%2zu] %-50s  %5u Hz  %u ch%s",
            i, narrow(d.friendly_name).c_str(),
            d.sample_rate, (unsigned)d.channels,
            d.is_default ? "  [DEFAULT]" : "");
    }
    log("");
    log("    --- CAPTURE (ingressi: microfoni e VB-CABLE Output) ---");
    for (size_t i = 0; i < devs.capture.size(); ++i)
    {
        const auto& d = devs.capture[i];
        log("    [c%2zu] %-50s  %5u Hz  %u ch%s",
            i, narrow(d.friendly_name).c_str(),
            d.sample_rate, (unsigned)d.channels,
            d.is_default ? "  [DEFAULT]" : "");
    }
    log("");
    if (devs.render.empty()) { log("    FAIL: no render devices"); ++failures; }

    log("[2] Open default render for 500 ms (silence)");
    const mixer::audio::AudioDevice* def = nullptr;
    for (const auto& d : devs.render) if (d.is_default) { def = &d; break; }
    if (!def && !devs.render.empty()) def = &devs.render.front();
    if (!def) { log("    SKIP: no render device"); ++failures; }
    else
    {
        log("    target: %s", narrow(def->friendly_name).c_str());
        mixer::audio::WasapiRenderStream s;
        std::atomic<uint64_t> total{0};
        std::atomic<uint32_t> cb{0};
        bool ok = s.start(def->id,
            [&](float* dst, uint32_t fr, uint32_t ch, uint32_t) {
                std::memset(dst, 0, (size_t)fr * ch * sizeof(float));
                total.fetch_add(fr, std::memory_order_relaxed);
                cb.fetch_add(1, std::memory_order_relaxed);
            });
        if (!ok)
        {
            log("    FAIL: start() returned false  HRESULT=0x%08lX (%s)",
                s.lastError(), hresultMessage(s.lastError()));
            ++failures;
        }
        else
        {
            log("    OK sr=%u ch=%u buf=%u",
                s.sampleRate(), s.channels(), s.bufferFrames());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            s.stop();
            log("    callbacks=%u total_frames=%llu",
                cb.load(), (unsigned long long)total.load());
            if (cb.load() == 0) { log("    FAIL: no callbacks fired"); ++failures; }
        }
    }

    log("[2b] Enum app sessions");
    try {
        auto sessions = mixer::audio::enumerateAppSessions();
        log("    sessions=%zu", sessions.size());
        for (const auto& s : sessions) {
            log("      pid=%u name=%s playing=%d override='%s'",
                s.process_id, narrow(s.process_name).c_str(),
                (int)s.is_playing, narrow(s.override_endpoint_id).c_str());
        }
    } catch (...) {
        log("    FAIL: enumerateAppSessions threw exception");
        ++failures;
    }

    log("[3] MixerConfig save/load roundtrip");
    {
        mixer::config::MixerCfg orig = mixer::config::makeDefaultConfig();
        orig.strips[0].label = "Mic prova";
        orig.strips[0].gain_db = -12.5f;
        orig.strips[0].mute = true;
        orig.strips[0].routes[0] = true;
        orig.strips[0].routes[1] = false;
        orig.strips[0].route_level[0] = 0.5f;   // invio al 50% sul bus 0
        orig.strips[0].route_level[1] = 1.0f;   // 100% sul bus 1
        orig.buses[1].label = "Stream";
        orig.buses[1].gain_db = -3.0f;

        const std::wstring path = L"smoke_roundtrip.mxp";
        if (!mixer::config::saveToFile(path, orig))
        {
            log("    FAIL: saveToFile failed"); ++failures;
        }
        else
        {
            mixer::config::MixerCfg loaded;
            if (!mixer::config::loadFromFile(path, loaded))
            {
                log("    FAIL: loadFromFile failed"); ++failures;
            }
            else
            {
                bool ok = true;
                if (loaded.strips.size() != orig.strips.size()) ok = false;
                if (loaded.buses.size()  != orig.buses.size())  ok = false;
                if (ok && loaded.strips[0].label != "Mic prova") ok = false;
                if (ok && std::fabs(loaded.strips[0].gain_db - (-12.5f)) > 1e-3f) ok = false;
                if (ok && !loaded.strips[0].mute) ok = false;
                if (ok && !loaded.strips[0].routes[0]) ok = false;
                if (ok &&  loaded.strips[0].routes[1]) ok = false;
                if (ok && (loaded.strips[0].route_level.size() < 2 ||
                           std::fabs(loaded.strips[0].route_level[0] - 0.5f) > 1e-3f ||
                           std::fabs(loaded.strips[0].route_level[1] - 1.0f) > 1e-3f))
                    ok = false;
                if (ok && loaded.buses[1].label != "Stream") ok = false;
                if (ok && std::fabs(loaded.buses[1].gain_db - (-3.0f)) > 1e-3f) ok = false;
                if (!ok) { log("    FAIL: roundtrip mismatch"); ++failures; }
                else log("    OK roundtrip preset");
            }
        }
    }

    log("");
    log("== RESULT: %s (failures=%d) ==",
        failures == 0 ? "PASS" : "FAIL", failures);
    fclose(f);
    return failures == 0 ? 0 : 1;
}

// ---- wWinMain --------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    {
        LPWSTR cmd = ::GetCommandLineW();
        if (cmd && wcsstr(cmd, L"--smoke"))
            return runSmokeTest();
    }

    enableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr,
                       ::LoadCursorW(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"MIXER", nullptr };
    ::RegisterClassExW(&wc);

    // 1) DPI del monitor su cui  il cursore. cursorMonitorDpi  pi
    //    affidabile di systemDpi() prima di CreateWindow (su Win11 26200
    //    GetDpiForSystem pu restituire 96 finch non esiste una window).
    UINT  dpi   = cursorMonitorDpi();
    if (dpi == 0) dpi = 96;
    float scale = (float)dpi / 96.0f;

    // 2) Desired client size in pixel fisici.
    const int client_w = (int)std::round(1280.0f * scale);
    const int client_h = (int)std::round(800.0f  * scale);

    // 3) Inflate per non-client area (titolo + bordi) al DPI corretto.
    RECT rc = { 0, 0, client_w, client_h };
    adjustWindowForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;

    // 4) Centra sul monitor primario.
    const int sw = ::GetSystemMetrics(SM_CXSCREEN);
    const int sh = ::GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - win_w) / 2; if (x < 0) x = 0;
    int y = (sh - win_h) / 2; if (y < 0) y = 0;

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MIXER",
                                WS_OVERLAPPEDWINDOW,
                                x, y, win_w, win_h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Re-read DPI ora che abbiamo l'hwnd. Riscalo SEMPRE la finestra alla DPI
    // reale del monitor su cui  finita, e la ricentro su quel monitor.
    // (Sempre, non condizionalmente: systemDpi() pu mentire su sistemi
    // multi-monitor con scaling miste, e l'utente si  ritrovato con una
    // finestra microscopica.)
    UINT actual_dpi = windowDpi(hwnd);
    if (actual_dpi == 0) actual_dpi = 96;
    const float ns = (float)actual_dpi / 96.0f;
    const int ncw = (int)std::round(1280.0f * ns);
    const int nch = (int)std::round(800.0f  * ns);
    RECT rc2 = { 0, 0, ncw, nch };
    adjustWindowForDpi(&rc2, WS_OVERLAPPEDWINDOW, FALSE, 0, actual_dpi);
    const int nw = rc2.right - rc2.left;
    const int nh = rc2.bottom - rc2.top;

    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    ::GetMonitorInfoW(mon, &mi);
    const int mw = mi.rcWork.right  - mi.rcWork.left;
    const int mh = mi.rcWork.bottom - mi.rcWork.top;
    int nx = mi.rcWork.left + (mw - nw) / 2; if (nx < mi.rcWork.left) nx = mi.rcWork.left;
    int ny = mi.rcWork.top  + (mh - nh) / 2; if (ny < mi.rcWork.top)  ny = mi.rcWork.top;
    dpi   = actual_dpi;
    scale = ns;

    // SetWindowPlacement: fissa la dimensione/posizione "normale" (restore)
    // per quando l'utente massimizzer + ripristiner.
    WINDOWPLACEMENT wp = { sizeof(wp) };
    wp.length             = sizeof(wp);
    wp.flags              = 0;
    wp.showCmd            = SW_SHOWNORMAL;
    wp.rcNormalPosition.left   = nx;
    wp.rcNormalPosition.top    = ny;
    wp.rcNormalPosition.right  = nx + nw;
    wp.rcNormalPosition.bottom = ny + nh;
    const BOOL placement_ok = ::SetWindowPlacement(hwnd, &wp);

    // Debug startup info su file.
    {
        FILE* lf = nullptr;
        fopen_s(&lf, "startup.log", "wb");
        if (lf)
        {
            std::fprintf(lf, "systemDpi=%u  windowDpi(hwnd)=%u  final_scale=%.2f\n",
                         systemDpi(), actual_dpi, ns);
            std::fprintf(lf, "monitor work: (%ld,%ld)-(%ld,%ld)  size %ldx%ld\n",
                         (long)mi.rcWork.left, (long)mi.rcWork.top,
                         (long)mi.rcWork.right, (long)mi.rcWork.bottom,
                         (long)(mi.rcWork.right - mi.rcWork.left),
                         (long)(mi.rcWork.bottom - mi.rcWork.top));
            std::fprintf(lf, "restore (normal) window outer = %dx%d  at (%d,%d)\n",
                         nw, nh, nx, ny);
            std::fprintf(lf, "SetWindowPlacement ok = %d  (started WINDOWED)\n",
                         (int)placement_ok);
            std::fclose(lf);
        }
    }

    // Mostra la finestra in modalit normale (windowed).
    // Limite noto: la dimensione che impostiamo via SetWindowPos/Placement
    // su hidden window viene spesso ignorata da Windows quando l'app  avviata
    // da PowerShell/console (STARTUPINFO interaction). Risultato: la finestra
    // parte alla dimensione di CreateWindow (1295x837 sul tuo monitor 4K).
    // L'utente pu ridimensionare/massimizzare manualmente se vuole pi grande.
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = "mixer_imgui.ini";

    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const float font_px = std::round(16.0f * scale);
        ImFont* loaded = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", font_px, &cfg,
            io.Fonts->GetGlyphRangesDefault());
        if (!loaded) io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.ScaleAllSizes(scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    const ImVec4 clear_color = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);

    auto devices = mixer::audio::enumerateAllDevices();

    TestToneState tone;
    mixer::audio::WasapiRenderStream test_stream;
    std::wstring test_tone_device_id;  // niente auto-default: l'utente sceglie

    mixer::config::MixerCfg cfg = mixer::config::makeDefaultConfig();
    mixer::audio::AudioEngine engine;
    MeterDisplayState meters;
    AppRoutingState app_routing;
    UiState ui;
    bool request_exit = false;

    // Controllo aggiornamenti in background (non blocca l'avvio). Se trova una
    // release piu' recente sul repo pubblico, RenderUpdateModal mostrera' il
    // prompt di conferma.
    mixer::update::Updater updater;
    UpdateUiState update_ui;
    updater.checkAsync(MIXER_VERSION);

    // Prima esecuzione: crea Documenti\MIXER\Preset e ci copia i preset di
    // esempio installati (l'utente li trovera' in "Apri preset").
    seedPresetsFromInstall();

    // All'avvio il preset e' VERGINE, a meno che l'utente non ne abbia impostato
    // uno come predefinito (pulsante "Imposta come Predefinito" -> settings.ini).
    ui.default_preset_path = loadDefaultPresetSetting();
    bool preset_loaded = false;
    if (!ui.default_preset_path.empty())
    {
        mixer::config::MixerCfg loaded;
        if (mixer::config::loadFromFile(ui.default_preset_path, loaded))
        {
            cfg = std::move(loaded);
            cfg.normalize();
            ui.current_preset_path = ui.default_preset_path;
            preset_loaded = true;
        }
        else
        {
            ui.default_preset_path.clear();  // il file non esiste piu': torna a vergine
        }
    }

    // Avvio automatico del motore mixer all'apertura dell'app.
    // L'utente pu sempre fermarlo col bottone rosso "Ferma mixer".
    if (preset_loaded)
    {
        if (engine.start(cfg, ui.buffer_ms, ui.exclusive_mode, ui.lowlat_mode))
            setStatus(ui, "Motore mixer avviato automaticamente all'apertura.", 5.0);
        else
            setStatus(ui, "Avvio automatico motore fallito: controlla i device "
                          "del preset e premi 'Avvia mixer'.", 8.0);
    }

    while (!request_exit)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) request_exit = true;
        }
        if (request_exit) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        bool request_refresh = false;
        RenderMenuBar(ui, cfg, test_stream, hwnd, request_refresh, request_exit,
                      updater, update_ui);
        RenderMixerPanel(&ui.show_mixer, cfg, devices, ui, tone, test_stream,
                         test_tone_device_id, engine, meters, app_routing);
        // Push live parameter changes (gain, mute, solo, routes) all'engine
        // se sta girando, cos i fader rispondono in tempo reale.
        engine.setBypassGains(!ui.enable_dbfs);
        if (engine.running())
            engine.updateParams(cfg);
        RenderStatusBar(ui, devices, cfg, test_stream);
        RenderDevicePanel(&ui.show_devices, devices);
        RenderWelcomePanel(&ui.show_welcome);
        RenderAboutPanel(&ui.show_about);
        RenderUpdateModal(updater, update_ui, request_exit);

        // Scorciatoie da tastiera per save/open
        ImGuiIO& kio = ImGui::GetIO();
        if (kio.KeyCtrl && !kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O))
        {
            std::wstring p = openPresetDialog(hwnd);
            if (!p.empty())
            {
                mixer::config::MixerCfg L;
                if (mixer::config::loadFromFile(p, L))
                {
                    cfg = std::move(L);
                    cfg.normalize();
                    ui.current_preset_path = p;
                    setStatus(ui, "Preset caricato.");
                }
                else setStatus(ui, "Errore nel caricamento del preset.");
            }
        }
        if (kio.KeyCtrl && !kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            if (!ui.current_preset_path.empty())
            {
                if (mixer::config::saveToFile(ui.current_preset_path, cfg))
                    setStatus(ui, "Preset salvato.");
            }
            else
            {
                std::wstring p = savePresetDialog(hwnd, L"");
                if (!p.empty() && mixer::config::saveToFile(p, cfg))
                {
                    ui.current_preset_path = p;
                    setStatus(ui, "Preset salvato.");
                }
            }
        }
        if (kio.KeyCtrl && kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            std::wstring p = savePresetDialog(hwnd, ui.current_preset_path);
            if (!p.empty() && mixer::config::saveToFile(p, cfg))
            {
                ui.current_preset_path = p;
                setStatus(ui, "Preset salvato.");
            }
        }
        if (request_refresh
            || app_routing.device_refresh_needed
            || ImGui::IsKeyPressed(ImGuiKey_F5))
        {
            devices = mixer::audio::enumerateAllDevices();
            app_routing.device_refresh_needed = false;
        }

        ImGui::Render();
        const float cc[4] = { clear_color.x, clear_color.y,
                              clear_color.z, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    engine.stop();
    test_stream.stop();

    // Attendi l'eventuale download di aggiornamento ancora in corso, cosi' il
    // thread non resta joinable alla distruzione (std::terminate).
    if (update_ui.worker.joinable()) update_ui.worker.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ---- D3D + WndProc boilerplate --------------------------------------------

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, levels, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            flags, levels, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBack = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRenderTargetView);
    pBack->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_DPICHANGED:
    {
        const RECT* rcc = (const RECT*)lParam;
        ::SetWindowPos(hWnd, nullptr, rcc->left, rcc->top,
                       rcc->right - rcc->left, rcc->bottom - rcc->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
