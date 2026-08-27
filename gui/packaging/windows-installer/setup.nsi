; SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
; SPDX-License-Identifier: GPL-3.0-or-later

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "x64.nsh"

!ifndef Stage
  !error "Stage must point to the deployed Windows application tree."
!endif
!ifndef AppVersion
  !error "AppVersion must contain the release version."
!endif
!ifndef AssetsDirectory
  !error "AssetsDirectory must point to the viewer icon directory."
!endif
!ifndef LicenseFile
  !error "LicenseFile must point to the viewer license."
!endif
!ifndef InstallerOutput
  !error "InstallerOutput must contain the output executable path."
!endif

!define ProductName "P2000T VID2VGA Capture"
!define ProductId "nl.ivofilot.p2000t.vid2vga.capture"
!define ProductRegistryKey "Software\Ivo Filot\P2000T VID2VGA Capture"
!define UninstallRegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\${ProductId}"

Name "${ProductName} ${AppVersion}"
OutFile "${InstallerOutput}"
InstallDir "$PROGRAMFILES64\P2000T VID2VGA Capture"
InstallDirRegKey HKLM "${ProductRegistryKey}" "InstallLocation"
BrandingText "P2000T VID2VGA Capture"
Icon "${AssetsDirectory}\p2000t-capture-v2.ico"
UninstallIcon "${AssetsDirectory}\p2000t-capture-v2.ico"
VIProductVersion "${AppVersion}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${ProductName}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${AppVersion}"
VIAddVersionKey /LANG=1033 "FileVersion" "${AppVersion}"
VIAddVersionKey /LANG=1033 "CompanyName" "Ivo Filot"
VIAddVersionKey /LANG=1033 "FileDescription" "${ProductName} Setup"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright 2026 Ivo Filot"

!define MUI_ABORTWARNING
!define MUI_ICON "${AssetsDirectory}\p2000t-capture-v2.ico"
!define MUI_UNICON "${AssetsDirectory}\p2000t-capture-v2.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\p2000t-vid2vga-capture.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LicenseFile}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${ProductName} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Install"
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"

  File /r "${Stage}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateDirectory "$SMPROGRAMS\P2000T VID2VGA Capture"
  CreateShortcut "$SMPROGRAMS\P2000T VID2VGA Capture\P2000T VID2VGA Capture.lnk" "$INSTDIR\p2000t-vid2vga-capture.exe" "" "$INSTDIR\p2000t-vid2vga-capture.exe"
  CreateShortcut "$DESKTOP\P2000T VID2VGA Capture.lnk" "$INSTDIR\p2000t-vid2vga-capture.exe" "" "$INSTDIR\p2000t-vid2vga-capture.exe"

  WriteRegStr HKLM "${ProductRegistryKey}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayName" "${ProductName}"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayVersion" "${AppVersion}"
  WriteRegStr HKLM "${UninstallRegistryKey}" "Publisher" "Ivo Filot"
  WriteRegStr HKLM "${UninstallRegistryKey}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayIcon" "$INSTDIR\p2000t-vid2vga-capture.exe"
  WriteRegStr HKLM "${UninstallRegistryKey}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKLM "${UninstallRegistryKey}" "QuietUninstallString" '$\"$INSTDIR\uninstall.exe$\" /S'
  WriteRegStr HKLM "${UninstallRegistryKey}" "URLInfoAbout" "https://github.com/ifilot/p2000t-video-to-vga-adapter"
  WriteRegDWORD HKLM "${UninstallRegistryKey}" "NoModify" 1
  WriteRegDWORD HKLM "${UninstallRegistryKey}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  Delete "$DESKTOP\P2000T VID2VGA Capture.lnk"
  RMDir /r "$SMPROGRAMS\P2000T VID2VGA Capture"
  SetShellVarContext all
  Delete "$DESKTOP\P2000T VID2VGA Capture.lnk"
  RMDir /r "$SMPROGRAMS\P2000T VID2VGA Capture"
  DeleteRegKey HKLM "${UninstallRegistryKey}"
  DeleteRegKey HKLM "${ProductRegistryKey}"
  RMDir /r "$INSTDIR"
SectionEnd
