; Setup wraps exactly the portable payload; service/headset ownership stays in Dashboard.
Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
Name "SurfXR"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\SurfXR"
InstallDirRegKey HKCU "Software\SurfXR" "InstallDir"
BrandingText "SurfXR ${RELEASE_ID}"
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!define APPKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SurfXR"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\SurfXR Dashboard.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "SurfXR requires 64-bit Windows."
        Abort
    ${EndIf}
    SetShellVarContext current
    !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Section "SurfXR"
    SetOutPath "$INSTDIR"
    File /r "${PAYLOAD}\*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\SurfXR" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${APPKEY}" "DisplayName" "SurfXR"
    WriteRegStr HKCU "${APPKEY}" "DisplayVersion" "${RELEASE_ID}"
    WriteRegStr HKCU "${APPKEY}" "Publisher" "SurfXR"
    WriteRegStr HKCU "${APPKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${APPKEY}" "DisplayIcon" "$INSTDIR\SurfXR Dashboard.exe"
    WriteRegStr HKCU "${APPKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${APPKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${APPKEY}" "NoModify" 1
    WriteRegDWORD HKCU "${APPKEY}" "NoRepair" 1
    CreateDirectory "$SMPROGRAMS\SurfXR"
    CreateShortcut "$SMPROGRAMS\SurfXR\SurfXR.lnk" "$INSTDIR\SurfXR Dashboard.exe"
    CreateShortcut "$SMPROGRAMS\SurfXR\Install Steam Frame.lnk" "$INSTDIR\Install-SteamFrame.cmd"
    CreateShortcut "$SMPROGRAMS\SurfXR\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    ; Preserve session.json, language preferences, logs and any user-added files.
    !include "${REMOVE_PAYLOAD}"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    ReadRegStr $0 HKCU "Software\SurfXR" "InstallDir"
    ${If} $0 == $INSTDIR
        DeleteRegKey HKCU "Software\SurfXR"
        DeleteRegKey HKCU "${APPKEY}"
        Delete "$SMPROGRAMS\SurfXR\SurfXR.lnk"
        Delete "$SMPROGRAMS\SurfXR\Install Steam Frame.lnk"
        Delete "$SMPROGRAMS\SurfXR\Uninstall.lnk"
        RMDir "$SMPROGRAMS\SurfXR"
    ${EndIf}
SectionEnd
