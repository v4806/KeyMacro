; 按键宏 (KeyMacro) Inno Setup 6 安装脚本
; 构建前准备：把编译出的 KeyMacro.exe 与 Qt/OpenCV 运行库放到 ..\dist\KeyMacro\
; 编译：iscc installer\KeyMacro.iss

#define MyAppName "按键宏"
#define MyAppVersion "4.8.0"
#define MyAppPublisher "v4806"
#define MyAppURL "https://github.com/v4806/KeyMacro"
#define MyAppExeName "KeyMacro.exe"

[Setup]
AppId={{7C4B2A91-6E3D-4F5A-9B8C-1D2E3F4A5B60}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; 沿用旧版本安装目录
DefaultDirName=D:\Program Files\按键宏
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist-output
OutputBaseFilename=KeyMacro-{#MyAppVersion}-Setup
SetupIconFile=..\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; 安装前提示关闭正在运行的按键宏，安装后不自动重启
CloseApplications=yes
RestartApplications=no
; 覆盖安装时保留用户已有的 macros.ini / image / config（安装包不含这些文件）

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; 程序文件与运行库（不含用户数据）
Source: "..\dist\KeyMacro\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
