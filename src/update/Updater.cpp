// ============================================================================
//  Updater  implementazione (WinHTTP + parsing JSON minimale).
// ============================================================================
#include "update/Updater.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstdio>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace mixer::update {
namespace {

// Repository PUBBLICO da cui leggere le release. Se rinomini il repo, cambia
// solo questa riga.
constexpr wchar_t kApiHost[] = L"api.github.com";
// Repo unico (codice + release). Le vecchie installazioni puntavano a
// "MIXER-Releases": GitHub redirige automaticamente al nuovo nome, e l'updater
// segue i redirect, quindi continuano ad aggiornarsi.
constexpr wchar_t kApiPath[] =
    L"/repos/WaxStefanoMusic/MIXER/releases/latest";

// ── Conversioni UTF-8 <-> UTF-16 ────────────────────────────────────────────
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ── RAII per gli handle WinHTTP ─────────────────────────────────────────────
struct Inet {
    HINTERNET h = nullptr;
    Inet() = default;
    explicit Inet(HINTERNET p) : h(p) {}
    ~Inet() { if (h) ::WinHttpCloseHandle(h); }
    Inet(const Inet&) = delete;
    Inet& operator=(const Inet&) = delete;
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

// GET di una URL. Segue i redirect (default di WinHTTP).
//   - se `buf`     != nullptr: accumula la risposta come testo (per l'API)
//   - se `outFile` non vuoto:  scrive la risposta su file binario (download)
// Ritorna true su HTTP 200.
bool httpFetch(const std::wstring& url, std::string* buf,
               const std::wstring& outFile, std::string& err) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    uc.lpszHostName = host;  uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = 4095;
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = "URL non valido";
        return false;
    }

    Inet session(::WinHttpOpen(L"MIXER-Updater/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { err = "WinHttpOpen fallita"; return false; }

    // Timeout ragionevoli: il controllo non deve mai bloccare l'avvio a lungo.
    ::WinHttpSetTimeouts(session, 8000, 8000, 12000, 30000);

    Inet connect(::WinHttpConnect(session, host, uc.nPort, 0));
    if (!connect) { err = "WinHttpConnect fallita"; return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Inet request(::WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) { err = "WinHttpOpenRequest fallita"; return false; }

    // L'API GitHub richiede un Accept esplicito; lo User-Agent e' quello di
    // WinHttpOpen ("MIXER-Updater/1.0"), obbligatorio per GitHub.
    ::WinHttpAddRequestHeaders(request,
                               L"Accept: application/vnd.github+json",
                               (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = "Invio richiesta fallito";
        return false;
    }
    if (!::WinHttpReceiveResponse(request, nullptr)) {
        err = "Nessuna risposta dal server";
        return false;
    }

    DWORD status = 0, slen = sizeof(status);
    ::WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = "HTTP " + std::to_string(status);
        return false;
    }

    FILE* f = nullptr;
    if (!outFile.empty()) {
        if (::_wfopen_s(&f, outFile.c_str(), L"wb") != 0 || !f) {
            err = "Impossibile creare il file temporaneo";
            return false;
        }
    }

    bool ok = true;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
        if (avail == 0) break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), avail, &read)) { ok = false; break; }
        if (read == 0) break;
        if (buf) buf->append(chunk.data(), read);
        if (f)   ::fwrite(chunk.data(), 1, read, f);
    }
    if (f) ::fclose(f);
    if (!ok) err = "Errore durante la lettura della risposta";
    return ok;
}

// ── Parsing JSON minimale (solo i campi che ci servono) ─────────────────────
// Legge una stringa JSON che inizia all'apertura virgolette `js[start]=='"'`.
// Ritorna il valore decodificato e aggiorna `start` oltre la chiusura.
std::string readJsonString(const std::string& js, size_t& start) {
    std::string out;
    size_t p = start;
    if (p >= js.size() || js[p] != '"') return out;
    ++p;
    while (p < js.size() && js[p] != '"') {
        if (js[p] == '\\' && p + 1 < js.size()) {
            char c = js[p + 1];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                default:  out += c;    break;  // \uXXXX non gestito: raro qui
            }
            p += 2;
            continue;
        }
        out += js[p++];
    }
    if (p < js.size()) ++p;  // salta la virgoletta di chiusura
    start = p;
    return out;
}

// Estrae il valore stringa della prima occorrenza di "key": "...".
std::string jsonField(const std::string& js, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return {};
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t' ||
                             js[p] == '\n' || js[p] == '\r')) ++p;
    if (p >= js.size() || js[p] != '"') return {};
    return readJsonString(js, p);
}

bool endsWithExe(const std::string& s) {
    if (s.size() < 4) return false;
    std::string tail = s.substr(s.size() - 4);
    for (char& c : tail) c = (char)::tolower((unsigned char)c);
    return tail == ".exe";
}

// Cerca tra gli asset il primo browser_download_url che termina con .exe.
std::string findExeAsset(const std::string& js) {
    const std::string key = "\"browser_download_url\"";
    size_t p = 0;
    while ((p = js.find(key, p)) != std::string::npos) {
        size_t c = js.find(':', p + key.size());
        if (c == std::string::npos) break;
        ++c;
        while (c < js.size() && (js[c] == ' ' || js[c] == '\t' ||
                                 js[c] == '\n' || js[c] == '\r')) ++c;
        std::string url = readJsonString(js, c);
        if (endsWithExe(url)) return url;
        p = c;
    }
    return {};
}

// ── Confronto versioni semantiche ───────────────────────────────────────────
std::vector<int> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size()) {
        int n = 0;
        bool any = false;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            n = n * 10 + (v[i] - '0');
            any = true;
            ++i;
        }
        if (any) parts.push_back(n);
        // salta separatori (. - + ecc.); fermati al primo non-numerico dopo
        while (i < v.size() && (v[i] < '0' || v[i] > '9')) ++i;
    }
    return parts;
}

// Ritorna >0 se a > b, <0 se a < b, 0 se uguali.
int compareVersions(const std::string& a, const std::string& b) {
    std::vector<int> pa = versionParts(a), pb = versionParts(b);
    size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (size_t i = 0; i < n; ++i) {
        int va = i < pa.size() ? pa[i] : 0;
        int vb = i < pb.size() ? pb[i] : 0;
        if (va != vb) return va - vb;
    }
    return 0;
}

}  // namespace

// ── API pubblica ────────────────────────────────────────────────────────────
void Updater::checkAsync(const std::string& current_version) {
    State expected = State::Idle;
    // Evita doppi avvii.
    if (!state_.compare_exchange_strong(expected, State::Checking)) {
        if (state_.load() == State::Error || state_.load() == State::UpToDate)
            state_.store(State::Checking, std::memory_order_release);
        else
            return;
    }
    current_version_ = current_version;

    std::thread([this, current_version]() {
        std::string body, err;
        const std::wstring url =
            std::wstring(L"https://") + kApiHost + kApiPath;
        if (!httpFetch(url, &body, L"", err)) {
            error_ = err;
            state_.store(State::Error, std::memory_order_release);
            return;
        }

        const std::string tag = jsonField(body, "tag_name");
        if (tag.empty()) {
            error_ = "Risposta GitHub senza tag_name";
            state_.store(State::Error, std::memory_order_release);
            return;
        }

        if (compareVersions(tag, current_version) <= 0) {
            state_.store(State::UpToDate, std::memory_order_release);
            return;
        }

        UpdateInfo nfo;
        nfo.latest_version = tag;
        if (!nfo.latest_version.empty() &&
            (nfo.latest_version[0] == 'v' || nfo.latest_version[0] == 'V'))
            nfo.latest_version.erase(0, 1);
        nfo.download_url   = findExeAsset(body);
        nfo.release_notes  = jsonField(body, "body");

        if (nfo.download_url.empty()) {
            error_ = "Nessun installer .exe trovato nella release";
            state_.store(State::Error, std::memory_order_release);
            return;
        }

        info_ = std::move(nfo);
        state_.store(State::Available, std::memory_order_release);
    }).detach();
}

bool Updater::downloadAndRun() {
    if (state_.load() != State::Available) return false;

    wchar_t tmpDir[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmpDir);
    std::wstring file = std::wstring(tmpDir) + L"MIXER_Setup_" +
                        widen(info_.latest_version) + L".exe";

    std::string err;
    if (!httpFetch(widen(info_.download_url), nullptr, file, err)) {
        error_ = err;
        return false;
    }

    // L'installer Inno e' admin-required: ShellExecute "open" fa scattare
    // l'UAC. /SILENT mostra solo la barra di avanzamento (l'utente ha gia'
    // confermato nel nostro modal). /CLOSEAPPLICATIONS chiude l'app in uso
    // cosi' l'exe puo' essere sostituito; /RESTARTAPPLICATIONS la riavvia.
    HINSTANCE r = ::ShellExecuteW(
        nullptr, L"open", file.c_str(),
        L"/SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART",
        nullptr, SW_SHOWNORMAL);

    if ((INT_PTR)r <= 32) {
        error_ = "Avvio dell'installer fallito";
        return false;
    }
    return true;
}

}  // namespace mixer::update
