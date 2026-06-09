; ============================================================================
;  MIXER  Inno Setup script
;  Installer con elevazione amministratore.
;  Bundla: mixer.exe (runtime statico, nessun VC++ Redist necessario),
;          i preset .mxp, e d3dcompiler_47.dll come fallback.
; ============================================================================

#define MyAppName        "MIXER"
; La versione puo' essere passata dalla riga di comando in CI:
;   ISCC.exe /DMyAppVersion=1.2.0 mixer.iss
; In assenza, default coerente con CMakeLists.txt.
#ifndef MyAppVersion
  #define MyAppVersion   "1.0.0"
#endif
#define MyAppPublisher   "MIXER"
#define MyAppExeName     "mixer.exe"

; I path Source/relativi qui sotto si risolvono rispetto alla cartella di
; QUESTO script (installer\). Lo script vive in installer\, quindi il root del
; repo e' "..". Tutto e' relativo: l'installer compila ovunque (CI inclusa),
; non solo su B:\MIXER. La CI puo' sovrascrivere BuildDir se serve.
#ifndef BuildDir
  #define BuildDir       "..\build"
#endif

[Setup]
AppId={{4F2C9A11-7B3E-4D8A-9C1F-MIXERAUDIO001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=MIXER_Setup_{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Elevazione amministratore OBBLIGATORIA per tutto l'installer.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\src\mixer.ico
SetupLogging=yes
; --- Aspetto / DPI elevati (4K) -------------------------------------------
; Lo stile modern e l'auto-scaling DPI di Inno Setup adattano gia' il wizard;
; qui forniamo immagini ad alta risoluzione (generate da make_wizard_images.ps1)
; cosi' restano nitide quando Setup scala a 150/200/250%, e fissiamo la
; dimensione del wizard per coerenza anche su versioni di Inno piu' datate.
; NB: NON usare WizardResizable, rimossa in Inno Setup 6.6.
; L'immagine grande e' gia' in proporzione 164:314: con lo stretch di default
; (che da Inno 6.4 mantiene l'aspect ratio) riempie il banner senza distorcere.
WizardSizePercent=120
WizardImageFile=wizard_large.png
WizardSmallImageFile=wizard_small.png
; Aggiornamento in-place: chiudi l'app in esecuzione (via Restart Manager) cosi'
; mixer.exe puo' essere sostituito, poi riavviala. Funziona con i flag passati
; dall'updater (/CLOSEAPPLICATIONS /RESTARTAPPLICATIONS).
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
; Eseguibile principale (runtime MSVC statico = nessuna dipendenza esterna)
Source: "{#BuildDir}\bin\Release\mixer.exe"; DestDir: "{app}"; Flags: ignoreversion
; d3dcompiler_47.dll  fallback: presente su Win10/11 ma lo bundliamo per
; massima sicurezza su sistemi con install minimale.
Source: "C:\Windows\System32\d3dcompiler_47.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Preset salvati (cartella accanto all'exe  l'app li cerca qui per primo)
Source: "..\Preset Salvati\*.mxp"; DestDir: "{app}\Preset Salvati"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\Disinstalla {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Avvio facoltativo a fine installazione. runasoriginaluser: l'app NON deve
; girare come admin (WASAPI/per-app routing funzionano meglio in user space).
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
; Rimuovi anche eventuali ini/log generati a runtime accanto all'exe.
Type: files; Name: "{app}\mixer_imgui.ini"
Type: files; Name: "{app}\smoke.log"
Type: files; Name: "{app}\startup.log"
Type: dirifempty; Name: "{app}\Preset Salvati"
Type: dirifempty; Name: "{app}"
