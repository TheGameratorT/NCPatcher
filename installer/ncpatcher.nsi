; NSIS installer for NCPatcher.
;
; Driven from build_installer.cmake, which passes every path/version in via /D:
;   VERSION        x.y.z (also used for the exe's version resource)
;   VIVERSION      x.y.z.0 (VIProductVersion needs four fields)
;   STAGE_DIR      a portable `cmake --install` of the build, native path
;   UNINSTALL_LIST an include of Delete/RMDir lines for every staged file
;   LICENSE_FILE   the GPLv3 text shown on the license page
;   ICON           the .ico used for both the installer and uninstaller
;   OUTFILE        where to write the Setup exe
;
; The optional toolchain is not carried in the installer. It is downloaded
; from Arm when its component is ticked, checked against the hash below, and
; unpacked beside NCPatcher. curl.exe and tar.exe have shipped with Windows
; since Windows 10 1803, so this needs no NSIS plugins.

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef VIVERSION
  !define VIVERSION "0.0.0.0"
!endif

!define TOOLCHAIN_VERSION "15.3.rel2"
!define TOOLCHAIN_URL "https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel2/arm-gnu-toolchain-15.3.rel2-mingw-w64-x86_64-arm-none-eabi.zip"
!define TOOLCHAIN_SHA256 "0daff4141d4f62ad1e9794ca8bd430ef730f07fd78f6d71d2ffdffc21db8cb9e"
!define TOOLCHAIN_DIR "$INSTDIR\arm-none-eabi"

!define REG_KEY "Software\NCPatcher"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NCPatcher"
!define ENV_KEY "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
!define POWERSHELL "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"

Name "NCPatcher"
BrandingText "NCPatcher ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\NCPatcher"
InstallDirRegKey HKLM "${REG_KEY}" "InstallDir"
RequestExecutionLevel admin

VIProductVersion "${VIVERSION}"
VIAddVersionKey "ProductName" "NCPatcher"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "TheGameratorT"
VIAddVersionKey "LegalCopyright" "GNU General Public License v3"
VIAddVersionKey "FileDescription" "NCPatcher Setup"
VIAddVersionKey "FileVersion" "${VERSION}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC

!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Adds DIR to the machine PATH, or removes it, through path.ps1. Expects the
; script to have been extracted into $PLUGINSDIR already.
!macro EditPath ACTION DIR
    nsExec::ExecToLog '"${POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\path.ps1" -Action ${ACTION} -Dir "${DIR}"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "Could not update PATH for ${DIR} (exit code $0)"
    ${EndIf}
!macroend

Section "NCPatcher" SecMain
    SectionIn RO

    ; A 32-bit installer would otherwise see SysWOW64 as $SYSDIR, and run the
    ; 32-bit PowerShell, curl and tar from there.
    ${DisableX64FSRedirection}

    SetOutPath "$INSTDIR"
    ; The whole portable install: the binary, the SDK files beside it, the
    ; schemas, and the docs.
    File /r "${STAGE_DIR}\*"

    InitPluginsDir
    File "/oname=$PLUGINSDIR\path.ps1" "${__FILEDIR__}\path.ps1"
    !insertmacro EditPath Add "$INSTDIR"

    ; For project templates and IDE configurations, which need to name ncp.h
    ; and its companions without knowing where this was installed.
    WriteRegExpandStr HKLM "${ENV_KEY}" "NCPATCHER_ROOT" "$INSTDIR"

    WriteRegStr HKLM "${REG_KEY}" "InstallDir" "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Add/Remove Programs entry.
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "NCPatcher"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "TheGameratorT"
    WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/TheGameratorT/NCPatcher"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\ncpatcher.exe"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Arm GNU Toolchain ${TOOLCHAIN_VERSION} (download)" SecToolchain
    ; What the toolchain takes once unpacked. The 276 MB download goes to the
    ; temporary directory and is deleted afterwards.
    AddSize 1097000

    ${DisableX64FSRedirection}

    ; Reinstalling or upgrading NCPatcher should not fetch the same toolchain
    ; again.
    ReadRegStr $0 HKLM "${REG_KEY}" "ToolchainVersion"
    ${If} $0 == "${TOOLCHAIN_VERSION}"
    ${AndIf} ${FileExists} "${TOOLCHAIN_DIR}\bin\arm-none-eabi-gcc.exe"
        DetailPrint "Arm GNU Toolchain ${TOOLCHAIN_VERSION} is already installed"
        Goto add_path
    ${EndIf}

    ${IfNot} ${FileExists} "$SYSDIR\curl.exe"
    ${OrIfNot} ${FileExists} "$SYSDIR\tar.exe"
        MessageBox MB_ICONEXCLAMATION|MB_OK "The toolchain could not be installed: this version of Windows has no curl.exe or tar.exe (Windows 10 version 1803 or later is needed).$\r$\n$\r$\nNCPatcher itself is installed. The toolchain can be downloaded by hand from:$\r$\n${TOOLCHAIN_URL}"
        Goto done
    ${EndIf}

    DetailPrint "Downloading Arm GNU Toolchain ${TOOLCHAIN_VERSION} (276 MB), this can take a while..."
    nsExec::ExecToLog '"$SYSDIR\curl.exe" --fail --location --silent --show-error --retry 3 --output "$PLUGINSDIR\toolchain.zip" "${TOOLCHAIN_URL}"'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_ICONEXCLAMATION|MB_OK "The toolchain could not be downloaded (curl exit code $0).$\r$\n$\r$\nNCPatcher itself is installed. Run the installer again to retry, or download the toolchain by hand from:$\r$\n${TOOLCHAIN_URL}"
        Goto done
    ${EndIf}

    DetailPrint "Verifying the download..."
    nsExec::ExecToLog `"${POWERSHELL}" -NoProfile -NonInteractive -Command "if ((Get-FileHash -Algorithm SHA256 -LiteralPath '$PLUGINSDIR\toolchain.zip').Hash -ne '${TOOLCHAIN_SHA256}') { exit 1 }"`
    Pop $0
    ${If} $0 != 0
        Delete "$PLUGINSDIR\toolchain.zip"
        MessageBox MB_ICONEXCLAMATION|MB_OK "The downloaded toolchain does not match its expected SHA-256 hash, so it was not installed.$\r$\n$\r$\nNCPatcher itself is installed. Run the installer again to retry."
        Goto done
    ${EndIf}

    ; Unpacked into a clean directory, so files a previous version had and this
    ; one does not are not left behind to be picked up.
    DetailPrint "Extracting the toolchain..."
    RMDir /r "${TOOLCHAIN_DIR}"
    CreateDirectory "${TOOLCHAIN_DIR}"
    nsExec::ExecToLog '"$SYSDIR\tar.exe" -xf "$PLUGINSDIR\toolchain.zip" -C "${TOOLCHAIN_DIR}"'
    Pop $0
    Delete "$PLUGINSDIR\toolchain.zip"
    ${If} $0 != 0
        RMDir /r "${TOOLCHAIN_DIR}"
        DeleteRegValue HKLM "${REG_KEY}" "ToolchainVersion"
        MessageBox MB_ICONEXCLAMATION|MB_OK "The toolchain could not be extracted (tar exit code $0).$\r$\n$\r$\nNCPatcher itself is installed. Run the installer again to retry."
        Goto done
    ${EndIf}

    WriteRegStr HKLM "${REG_KEY}" "ToolchainVersion" "${TOOLCHAIN_VERSION}"

add_path:
    !insertmacro EditPath Add "${TOOLCHAIN_DIR}\bin"

done:
SectionEnd

; Last, so that it runs after every PATH change above. Without it a terminal
; opened afterwards would not see the new variables until the next sign-in.
Section "-Broadcast"
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} \
        "NCPatcher and its SDK files. Added to PATH, with NCPATCHER_ROOT set to the install directory."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecToolchain} \
        "Downloads arm-none-eabi-gcc from Arm (276 MB) and adds it to PATH. Unticked when an arm-none-eabi-gcc is already on PATH."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP|MB_OK "This installer is for 64-bit Windows. Use the 32-bit zip release instead."
        Abort
    ${EndIf}

    ; Someone who already has a toolchain (devkitARM, or one installed by hand)
    ; should not be made to download a second one. One found in this install's
    ; own directory is the one being upgraded, so it stays ticked.
    SearchPath $0 "arm-none-eabi-gcc.exe"
    ${If} $0 != ""
    ${AndIf} $0 != "${TOOLCHAIN_DIR}\bin\arm-none-eabi-gcc.exe"
        SectionSetFlags ${SecToolchain} 0
    ${EndIf}
FunctionEnd

Section "Uninstall"
    ${DisableX64FSRedirection}

    InitPluginsDir
    File "/oname=$PLUGINSDIR\path.ps1" "${__FILEDIR__}\path.ps1"
    !insertmacro EditPath Remove "${TOOLCHAIN_DIR}\bin"
    !insertmacro EditPath Remove "$INSTDIR"

    ; Only if it still points here; another install may have taken it over.
    ReadRegStr $0 HKLM "${ENV_KEY}" "NCPATCHER_ROOT"
    ${If} $0 == "$INSTDIR"
        DeleteRegValue HKLM "${ENV_KEY}" "NCPATCHER_ROOT"
    ${EndIf}

    RMDir /r "${TOOLCHAIN_DIR}"

    ; Exactly the files this installer put there, rather than RMDir /r on
    ; $INSTDIR, which would take anything else the user had kept in it along.
    !include "${UNINSTALL_LIST}"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKLM "${UNINST_KEY}"
    DeleteRegKey HKLM "${REG_KEY}"

    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd
