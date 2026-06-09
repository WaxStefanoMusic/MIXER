// ============================================================================
//  Updater  controllo aggiornamenti via GitHub Releases (repo pubblico).
//
//  Flusso:
//    1. All'avvio l'app chiama checkAsync(MIXER_VERSION): parte un thread che
//       interroga l'API GitHub dell'ultima release del repo PUBBLICO.
//    2. Confronta la versione installata col tag dell'ultima release.
//    3. Se ne esiste una piu' recente, state() diventa Available e info()
//       contiene versione + URL dell'installer .exe.
//    4. La UI mostra un modal di conferma; su accettazione chiama
//       downloadAndRun(), che scarica l'installer in %TEMP% e lo esegue
//       (auto-elevazione via UAC). L'app deve poi chiudersi.
//
//  Implementazione: solo WinHTTP (nessuna dipendenza esterna, coerente con
//  l'exe a runtime statico).
// ============================================================================
#pragma once

#include <atomic>
#include <string>

namespace mixer::update {

struct UpdateInfo {
    std::string latest_version;  // es. "1.2.0" (senza prefisso 'v')
    std::string download_url;    // URL diretto dell'installer .exe
    std::string release_notes;   // corpo della release (puo' essere vuoto)
};

enum class State {
    Idle,       // mai avviato
    Checking,   // controllo in corso
    UpToDate,   // sei gia' all'ultima versione
    Available,  // c'e' un aggiornamento (vedi info())
    Error       // controllo fallito (vedi error())
};

// Owner stabile (vive per tutta la durata dell'app, allocato nello stack di
// WinMain). Non copiabile.
class Updater {
public:
    Updater() = default;
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Avvia il controllo in un thread detached (non bloccante).
    // current_version es. "1.0.0".
    void checkAsync(const std::string& current_version);

    State state() const { return state_.load(std::memory_order_acquire); }

    // Validi solo quando state() == Available.
    const UpdateInfo& info() const { return info_; }
    const std::string& error() const { return error_; }

    // Scarica l'installer dell'ultima release in %TEMP% e lo avvia.
    // Ritorna true se l'installer e' stato lanciato: in tal caso l'app
    // dovrebbe chiudersi per permettere la sostituzione dei file.
    // Bloccante (esegue il download); chiamala da un thread dedicato.
    bool downloadAndRun();

private:
    std::atomic<State> state_{State::Idle};
    UpdateInfo         info_;
    std::string        error_;
    std::string        current_version_;
};

}  // namespace mixer::update
