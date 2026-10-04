; NG64 installer (Inno Setup 6). Built by package.sh into dist\NG64-Setup.exe.
;
; What it does:
;  - asks for the player's own Super Mario 64 (US) ROM, any file name, and checks it's really that (size, byte
;    order, header); nothing from the game ships with NG64, the helper reads Mario, his textures, HUD and music from it
;  - puts the helper and a copy of the ROM in %LOCALAPPDATA%\NG64 (per user, no admin rights)
;  - puts the mod (ng64.zip) in BeamNG's mods folder, found from the registry / startup.ini, or picked by hand
;  - starts the helper's standby watcher now and at every sign-in: it launches the helper whenever BeamNG runs, and
;    the helper closes with the game (a BeamNG mod can't start programs itself)

#define AppName "NG64"
; package.sh passes the version from VERSION (/DAppVersion=...)
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#define Dist "..\dist"

[Setup]
AppId={{6F1E2B4C-9A37-4E8B-8C2D-5A64B2F0E064}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=NG64
DefaultDirName={localappdata}\NG64
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
OutputDir={#Dist}
OutputBaseFilename=NG64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} (Mario for BeamNG.drive)
UninstallDisplayIcon={app}\ng64helper.exe
CloseApplications=yes

[Messages]
WelcomeLabel2=This will install NG64, which lets you play as Mario in BeamNG.drive.%n%nYou'll need your own copy of Super Mario 64 (US version) as a ROM file. NG64 doesn't include anything from the game: Mario, his animations, textures, HUD and music are all read from your ROM.%n%nPlease close BeamNG.drive before continuing.

[Files]
Source: "{#Dist}\NG64\ng64helper.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\ng64.zip"; DestDir: "{code:GetModsDir}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\server\NG64\*"; DestDir: "{app}\beammp_server_plugin"; Flags: ignoreversion recursesubdirs

[Registry]
; where the mod and Mario's picture went, for the uninstaller (the wizard pages don't exist then)
Root: HKCU; Subkey: "Software\NG64"; ValueType: string; ValueName: "ModsDir"; ValueData: "{code:GetModsDir}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\NG64"; ValueType: string; ValueName: "UserDir"; ValueData: "{code:GetUserDir}"; Flags: uninsdeletekey
; the standby watcher, at every sign-in
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "NG64"; ValueData: """{app}\ng64helper.exe"" --watch"; Flags: uninsdeletevalue

[Icons]
Name: "{userprograms}\NG64\NG64"; Filename: "{app}\ng64helper.exe"; Parameters: "--watch"; Comment: "Starts NG64's helper if it isn't running (normally it starts by itself with Windows)"
Name: "{userprograms}\NG64\NG64 folder"; Filename: "{app}"
Name: "{userprograms}\NG64\Uninstall NG64"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\ng64helper.exe"; Parameters: "--watch"; Flags: nowait

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM ng64helper.exe"; Flags: runhidden; RunOnceId: "StopHelper"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
const
  ROM_SIZE = 8388608;

var
  RomPage: TInputFileWizardPage;
  ModsPage: TInputDirWizardPage;
  RomExt: String;          // .z64 / .v64 / .n64: the byte order the chosen ROM is in
  BeamNGFound: Boolean;

// ---- BeamNG's mods folder --------------------------------------------------------------------------------------

// BeamNG's user folder: its startup.ini can point it somewhere else, otherwise %LOCALAPPDATA%\BeamNG\BeamNG.drive
function BeamNGUserDir(): String;
var
  Root, Ini, UserPath: String;
begin
  Result := '';
  BeamNGFound := False;
  if RegQueryStringValue(HKCU, 'Software\BeamNG\BeamNG.drive', 'rootpath', Root) and DirExists(Root) then
  begin
    BeamNGFound := True;
    Ini := AddBackslash(Root) + 'startup.ini';
    if FileExists(Ini) then
    begin
      UserPath := Trim(GetIniString('filesystem', 'UserPath', '', Ini));
      if UserPath <> '' then
      begin
        if (Pos(':', UserPath) = 2) or (Copy(UserPath, 1, 2) = '\\') then
          Result := UserPath
        else
          Result := ExpandFileName(AddBackslash(Root) + UserPath);
      end;
    end;
  end;
  if Result = '' then
    Result := ExpandConstant('{localappdata}\BeamNG\BeamNG.drive');
  if DirExists(Result) then
    BeamNGFound := True;
end;

function DefaultModsDir(): String;
begin
  Result := AddBackslash(BeamNGUserDir()) + 'current\mods';
end;

function GetModsDir(Param: String): String;
begin
  Result := RemoveBackslashUnlessRoot(ModsPage.Values[0]);
end;

// BeamNG's user folder, the parent of mods: where Mario's vehicle selector picture goes
function GetUserDir(Param: String): String;
begin
  Result := ExtractFileDir(GetModsDir(''));
end;

// ---- the ROM ---------------------------------------------------------------------------------------------------

// byte i of the ROM as the N64 sees it (.z64 order), whatever order the file is in
function RomByte(const Data: AnsiString; I: Integer): Integer;
begin
  if RomExt = '.v64' then I := I xor 1
  else if RomExt = '.n64' then I := I xor 3;
  Result := Ord(Data[I + 1]);
end;

// '' if the file is a Super Mario 64 (US) ROM, otherwise what's wrong with it
function CheckRom(const FileName: String): String;
var
  Data: AnsiString;
  Title: String;
  I: Integer;
begin
  Result := '';
  if not FileExists(FileName) then begin Result := 'That file doesn''t exist.'; exit; end;
  if not LoadStringFromFile(FileName, Data) then begin Result := 'That file couldn''t be read.'; exit; end;
  if Length(Data) <> ROM_SIZE then
  begin
    Result := 'That isn''t a Super Mario 64 ROM (it should be exactly 8 MB).';
    exit;
  end;
  // byte order, from the first 4 bytes (80 37 12 40 in .z64 order)
  if (Ord(Data[1]) = $80) and (Ord(Data[2]) = $37) and (Ord(Data[3]) = $12) and (Ord(Data[4]) = $40) then RomExt := '.z64'
  else if (Ord(Data[1]) = $37) and (Ord(Data[2]) = $80) and (Ord(Data[3]) = $40) and (Ord(Data[4]) = $12) then RomExt := '.v64'
  else if (Ord(Data[1]) = $40) and (Ord(Data[2]) = $12) and (Ord(Data[3]) = $37) and (Ord(Data[4]) = $80) then RomExt := '.n64'
  else begin Result := 'That isn''t an N64 ROM.'; exit; end;
  // the header's game title and region
  Title := '';
  for I := $20 to $2D do Title := Title + Chr(RomByte(Data, I));
  if Title <> 'SUPER MARIO 64' then
  begin
    Result := 'That''s an N64 ROM, but not Super Mario 64.';
    exit;
  end;
  if RomByte(Data, $3E) <> Ord('E') then
  begin
    Result := 'That''s Super Mario 64, but not the US version. NG64 needs the US (NTSC-U) ROM.';
    exit;
  end;
end;

// ---- wizard ----------------------------------------------------------------------------------------------------

procedure InitializeWizard();
var
  Found: String;
begin
  RomPage := CreateInputFilePage(wpWelcome,
    'Your Super Mario 64 ROM', 'NG64 reads Mario from your own copy of the game.',
    'Choose your Super Mario 64 (US) ROM file. Any file name is fine; .z64, .v64 and .n64 dumps all work.');
  RomPage.Add('ROM file:', 'N64 ROMs (*.z64;*.v64;*.n64)|*.z64;*.v64;*.n64|All files|*.*', '.z64');
  // /ROM= and /MODSDIR= on the command line fill these in (for scripted / silent installs)
  RomPage.Values[0] := ExpandConstant('{param:ROM|}');

  ModsPage := CreateInputDirPage(RomPage.ID,
    'BeamNG.drive mods folder', 'Where the NG64 mod goes.', '', False, '');
  ModsPage.Add('');
  ModsPage.Values[0] := ExpandConstant('{param:MODSDIR|}');
  if ModsPage.Values[0] = '' then
    ModsPage.Values[0] := DefaultModsDir();
  if BeamNGFound then
    Found := 'BeamNG.drive was found and this is its mods folder. You normally don''t need to change it.'
  else
    Found := 'BeamNG.drive wasn''t found on this PC. Choose its mods folder (usually %LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods), or install BeamNG.drive first.';
  ModsPage.SubCaptionLabel.Caption := Found;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Problem: String;
begin
  Result := True;
  // a silent install can't be sent back to a page: PrepareToInstall checks the ROM and stops it instead
  if WizardSilent then exit;
  if CurPageID = RomPage.ID then
  begin
    Problem := CheckRom(RomPage.Values[0]);
    if Problem <> '' then
    begin
      MsgBox(Problem, mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = ModsPage.ID then
  begin
    if Trim(ModsPage.Values[0]) = '' then
    begin
      MsgBox('Choose BeamNG.drive''s mods folder.', mbError, MB_OK);
      Result := False;
    end
    else if not DirExists(ModsPage.Values[0]) then
      Result := MsgBox('That folder doesn''t exist yet. Create it and install the mod there?', mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Super Mario 64 ROM:' + NewLine + Space + RomPage.Values[0] + NewLine + NewLine +
            'NG64 mod goes in:' + NewLine + Space + ModsPage.Values[0] + NewLine + NewLine +
            'NG64''s helper goes in:' + NewLine + Space + ExpandConstant('{app}') + NewLine + NewLine +
            'The helper starts by itself with Windows and runs while BeamNG.drive does.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  // checked again here: a silent install never shows the ROM page
  Result := CheckRom(RomPage.Values[0]);
  if Result <> '' then
  begin
    Result := 'Super Mario 64 ROM: ' + Result;
    exit;
  end;
  // a running helper would keep its exe locked
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ng64helper.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Dest: String;
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    // the ROM next to the helper, named by its byte order (the helper reads any of them)
    DeleteFile(ExpandConstant('{app}\sm64.us.z64'));
    DeleteFile(ExpandConstant('{app}\sm64.us.v64'));
    DeleteFile(ExpandConstant('{app}\sm64.us.n64'));
    Dest := ExpandConstant('{app}\sm64.us') + RomExt;
    if not FileCopy(RomPage.Values[0], Dest, False) then
      MsgBox('Your ROM couldn''t be copied to ' + Dest + '. Copy it there yourself, or run the installer again.', mbError, MB_OK)
    else
      // Mario's picture for BeamNG's vehicle selector, drawn from the ROM that was just copied (nothing is shipped)
      Exec(ExpandConstant('{app}\ng64helper.exe'), '--write-preview "' + GetUserDir('') + '" --rom "' + Dest + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

// The wizard's pages don't exist when uninstalling, so the folders the install used come from the registry (written
// above). Runs before the uninstaller's own file and registry removal.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    if RegQueryStringValue(HKCU, 'Software\NG64', 'ModsDir', Dir) then
      DeleteFile(AddBackslash(Dir) + 'ng64.zip');
    if RegQueryStringValue(HKCU, 'Software\NG64', 'UserDir', Dir) then
    begin
      DelTree(AddBackslash(Dir) + 'vehicles\ng64_mario', True, True, True);
      DelTree(AddBackslash(Dir) + 'ng64_cache', True, True, True);
    end;
  end;
end;
