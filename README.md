# MIXER

Mixer audio software per Windows basato su **WASAPI** (shared mode), con UI in
**Dear ImGui** + Direct3D 11. Eseguibile singolo a runtime statico (`/MT`):
nessun Visual C++ Redistributable richiesto.

> Repository **pubblico unico**: codice sorgente, installer e aggiornamenti
> automatici stanno tutti qui. Scarica l'ultima versione dalla pagina
> [**Releases**](https://github.com/WaxStefanoMusic/MIXER/releases/latest).

## Build locale

Requisiti: Visual Studio (toolset MSVC C++) e CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target mixer
# -> build\bin\Release\mixer.exe
```

La versione e' un'unica fonte di verita': `project(MIXER VERSION x.y.z)` in
[CMakeLists.txt](CMakeLists.txt). Viene compilata nell'app come `MIXER_VERSION`
(mostrata in *Aiuto -> Informazioni*) e usata dall'updater per confrontarsi con
l'ultima release.

## Aggiornamenti automatici

All'avvio l'app controlla in background l'ultima release di questo repo
([src/update/Updater.cpp](src/update/Updater.cpp), solo WinHTTP). Se ne esiste
una piu' recente, mostra un prompt di conferma; accettando, scarica l'installer
in `%TEMP%` e lo esegue (UAC), poi si chiude per farsi sostituire.

## Pubblicare una release

1. Bump della versione in `CMakeLists.txt` (`project(... VERSION ...)`).
2. Commit e push su `main`.
3. Tag e push del tag:

   ```powershell
   git tag -a v1.2.0 -m "Note di rilascio mostrate nel prompt di aggiornamento"
   git push origin v1.2.0
   ```

4. La GitHub Action [release.yml](.github/workflows/release.yml) compila l'exe,
   costruisce l'installer Inno Setup e pubblica la release in questo stesso repo.

> Usa il `GITHUB_TOKEN` integrato di Actions (nessun PAT da configurare).

## Struttura

| Percorso | Contenuto |
|---|---|
| [src/audio/](src/audio/) | WASAPI render/capture, engine, sessioni, routing |
| [src/config/](src/config/) | Serializzazione preset `.mxp` |
| [src/update/](src/update/) | Auto-updater via GitHub Releases |
| [installer/mixer.iss](installer/mixer.iss) | Script Inno Setup (path relativi, versione da CI) |
| [.github/workflows/release.yml](.github/workflows/release.yml) | Build + pubblicazione release |
