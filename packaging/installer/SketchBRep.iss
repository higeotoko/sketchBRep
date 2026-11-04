; SketchBRep installer (Inno Setup 6). Built by CI from the portable folder
; (dist\SketchBRep): iscc /DAppVersion=0.1.0 /DSourceDir=...\dist\SketchBRep
; /DOutputDir=...\dist SketchBRep.iss
;
; - Installs for the current user (no administrator rights needed; an
;   administrator can choose all users), into Programs\SketchBRep.
; - Start menu entry, optional desktop icon, .sbr files open in SketchBRep.
; - Uninstalling removes the program only: the user's models, settings,
;   autosaves and crash records stay.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\SketchBRep"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{6A2F1C9E-5B7D-4E83-9C41-2D8F0B6E7A15}
AppName=SketchBRep
AppVersion={#AppVersion}
AppVerName=SketchBRep {#AppVersion}
AppPublisher=SketchBRep
DefaultDirName={autopf}\SketchBRep
DefaultGroupName=SketchBRep
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
ChangesAssociations=yes
UninstallDisplayIcon={app}\SketchBRep.exe
UninstallDisplayName=SketchBRep {#AppVersion}
OutputDir={#OutputDir}
OutputBaseFilename=SketchBRep-{#AppVersion}-setup-x64
SetupIconFile=..\..\assets\branding\05-windows\sketchbrep-compact.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "デスクトップにアイコンを作る"; GroupDescription: "追加のアイコン:"; Flags: unchecked
Name: "association"; Description: ".sbr ファイルを SketchBRep で開く"; GroupDescription: "ファイルの関連付け:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\SketchBRep"; Filename: "{app}\SketchBRep.exe"
Name: "{autoprograms}\SketchBRep をはじめる"; Filename: "{app}\00_START_HERE.md"
Name: "{autodesktop}\SketchBRep"; Filename: "{app}\SketchBRep.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.sbr"; ValueType: string; ValueName: ""; ValueData: "SketchBRep.Model"; Flags: uninsdeletevalue; Tasks: association
Root: HKA; Subkey: "Software\Classes\SketchBRep.Model"; ValueType: string; ValueName: ""; ValueData: "SketchBRep モデル"; Flags: uninsdeletekey; Tasks: association
Root: HKA; Subkey: "Software\Classes\SketchBRep.Model\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\SketchBRep.exe,0"; Tasks: association
Root: HKA; Subkey: "Software\Classes\SketchBRep.Model\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\SketchBRep.exe"" ""%1"""; Tasks: association

[Run]
Filename: "{app}\SketchBRep.exe"; Description: "SketchBRep を起動する"; Flags: nowait postinstall skipifsilent
