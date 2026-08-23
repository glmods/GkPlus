; Locates a Gunlok install through Steam, so the installer's Directory page can default to
; it instead of the usual "Program Files\GkPlus" - GkPlus is not its own application, it is a
; single DLL that has to land next to gl.exe.
;
; Two registry sources for the Steam install itself, tried in order:
;   - HKCU\Software\Valve\Steam\SteamPath - kept up to date by the Steam client for the
;     logged-in user, forward-slashed.
;   - HKLM\SOFTWARE\Valve\Steam\InstallPath - written at Steam's own install time. Steam is a
;     32-bit process, so on 64-bit Windows this key lives under Wow6432Node; SetRegView 32
;     reaches that view (and is a no-op on genuine 32-bit Windows, where there is no
;     redirection to begin with). SetRegView 64 is tried last in case a future Steam ever
;     registers natively there.
;
; From there, every Steam library can hold Gunlok, not just the one Steam itself lives in, so
; "<SteamPath>\steamapps\libraryfolders.vdf" is read for every "path" entry - Valve's own
; KeyValues format - and each candidate library is checked for "steamapps\common\Gunlok\gl.exe".
; The main Steam install is checked on its own first, because it is an implicit library and does
; not always get an entry of its own in that file.
;
; This is a line-oriented approximation of KeyValues, not a real parser: it looks for exactly the
; two quoted tokens on a "<key>"<whitespace>"<value>" line and only acts when the key is the
; literal string "path". That is enough for libraryfolders.vdf's shape and does not need to
; understand its nesting.
;
; Register convention through this whole file, since NSIS variables are all global and there are
; no real locals: $0-$6 hold state that must survive a Call (the caller's business); $7, $8, $9
; and $R0-$R9 are inputs/outputs/scratch for whichever function is running and are trashed by any
; Call. Nothing below this file's own functions may rely on $7-$9 or $R0-$R9 surviving a Call.

Var GkPlusDirSet

; in: $8 = raw path (may have forward slashes, and/or doubled backslashes from an unescaped VDF
;     value)
; out: $7 = the same path with a single backslash as every separator
Function GkPlus.NormalizePath
  StrCpy $7 ""
  StrLen $R0 $8
  StrCpy $R1 0

  gkplus_np_loop:
    IntCmp $R1 $R0 gkplus_np_done gkplus_np_cont gkplus_np_done
    gkplus_np_cont:
    StrCpy $R2 $8 1 $R1
    StrCmp $R2 "/" gkplus_np_slash gkplus_np_check
    gkplus_np_slash:
      StrCpy $R2 "\"
    gkplus_np_check:
    StrCmp $R2 "\" 0 gkplus_np_emit
      StrCpy $R3 $7 1 -1
      StrCmp $R3 "\" gkplus_np_skip
    gkplus_np_emit:
      StrCpy $7 "$7$R2"
    gkplus_np_skip:
    IntOp $R1 $R1 + 1
    Goto gkplus_np_loop
  gkplus_np_done:
FunctionEnd

; in: $8 = a line of text, $9 = which quoted token to extract (1 or 2)
; out: $7 = the token's contents, or "" if the line does not have one at that position
;
; Token 1 is whatever is between the line's 1st and 2nd quote character; token 2 is whatever is
; between its 3rd and 4th. On a "<key>"<ws>"<value>" line those are the key and the value.
Function GkPlus.QuotedToken
  StrCpy $7 ""
  StrLen $R0 $8
  StrCpy $R1 0    ; current character index
  StrCpy $R3 0    ; quotes seen so far
  StrCpy $R4 0    ; index right after the token's opening quote
  IntOp $R5 $9 - 1
  IntOp $R5 $R5 * 2
  IntOp $R5 $R5 + 1  ; ordinal of the opening quote for this token (1 or 3)
  IntOp $R6 $9 * 2   ; ordinal of the closing quote for this token (2 or 4)

  gkplus_qt_loop:
    IntCmp $R1 $R0 gkplus_qt_done gkplus_qt_cont gkplus_qt_done
    gkplus_qt_cont:
    StrCpy $R2 $8 1 $R1
    StrCmp $R2 '"' 0 gkplus_qt_next
      IntOp $R3 $R3 + 1
      IntCmp $R3 $R5 gkplus_qt_open gkplus_qt_checkclose gkplus_qt_checkclose
      gkplus_qt_checkclose:
      IntCmp $R3 $R6 gkplus_qt_close gkplus_qt_next gkplus_qt_next
      gkplus_qt_open:
        IntOp $R4 $R1 + 1
        Goto gkplus_qt_next
      gkplus_qt_close:
        IntOp $R2 $R1 - $R4
        StrCpy $7 $8 $R2 $R4
        Goto gkplus_qt_done
    gkplus_qt_next:
    IntOp $R1 $R1 + 1
    Goto gkplus_qt_loop
  gkplus_qt_done:
FunctionEnd

; out: $0 = the Gunlok install directory, or "" if it could not be found. $1-$6 are used as
;     working state and are not meaningful on return.
Function GkPlus.FindGunlokDir
  StrCpy $0 ""

  ReadRegStr $0 HKCU "Software\Valve\Steam" "SteamPath"
  StrCmp $0 "" 0 gkplus_have_steam

  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\Valve\Steam" "InstallPath"
  SetRegView lastused
  StrCmp $0 "" 0 gkplus_have_steam

  SetRegView 64
  ReadRegStr $0 HKLM "SOFTWARE\Valve\Steam" "InstallPath"
  SetRegView lastused
  StrCmp $0 "" gkplus_not_found gkplus_have_steam

  gkplus_have_steam:
  StrCpy $8 $0
  Call GkPlus.NormalizePath
  StrCpy $0 $7
  StrCpy $R0 $0 1 -1
  StrCmp $R0 "\" 0 +2
    StrCpy $0 $0 -1

  StrCpy $1 "$0\steamapps\common\Gunlok\gl.exe"
  IfFileExists "$1" gkplus_found_main gkplus_scan_vdf

  gkplus_found_main:
  StrCpy $0 "$0\steamapps\common\Gunlok"
  Goto gkplus_done

  gkplus_scan_vdf:
  ClearErrors
  FileOpen $3 "$0\steamapps\libraryfolders.vdf" r
  IfErrors gkplus_not_found

  gkplus_read_loop:
    ClearErrors
    FileRead $3 $2
    IfErrors gkplus_close_not_found

    StrCpy $8 $2
    StrCpy $9 1
    Call GkPlus.QuotedToken
    StrCmp $7 "path" 0 gkplus_read_loop

    StrCpy $8 $2
    StrCpy $9 2
    Call GkPlus.QuotedToken
    StrCmp $7 "" gkplus_read_loop

    StrCpy $8 $7
    Call GkPlus.NormalizePath
    StrCpy $R0 $7 1 -1
    StrCmp $R0 "\" 0 +2
      StrCpy $7 $7 -1

    StrCpy $1 "$7\steamapps\common\Gunlok\gl.exe"
    IfFileExists "$1" gkplus_found_library gkplus_read_loop

    gkplus_found_library:
    StrCpy $0 "$7\steamapps\common\Gunlok"
    FileClose $3
    Goto gkplus_done

  gkplus_close_not_found:
  FileClose $3

  gkplus_not_found:
  StrCpy $0 ""

  gkplus_done:
FunctionEnd

; MUI_PAGE_CUSTOMFUNCTION_PRE for the Directory page: runs detection once, the first time that
; page is about to show, and defaults $INSTDIR to whatever it found. Gated on $GkPlusDirSet so a
; manual edit survives paging Back and then Next again - only the untouched default gets
; overwritten.
Function GkPlus.DirectoryPre
  StrCmp $GkPlusDirSet "1" gkplus_predone
  StrCpy $GkPlusDirSet "1"
  Call GkPlus.FindGunlokDir
  StrCmp $0 "" gkplus_predone
    StrCpy $INSTDIR "$0"
  gkplus_predone:
FunctionEnd

; MUI_PAGE_CUSTOMFUNCTION_LEAVE for the Directory page: warns rather than blocks, since the whole
; point is only to catch detection having failed silently or the user having browsed somewhere
; that plainly isn't Gunlok's folder.
Function GkPlus.DirectoryLeave
  IfFileExists "$INSTDIR\gl.exe" gkplus_leave_ok
  MessageBox MB_YESNO|MB_ICONEXCLAMATION \
    "$INSTDIR$\r$\ndoes not look like a Gunlok install (no gl.exe there).$\r$\nInstall d3d8.dll there anyway?" \
    IDYES gkplus_leave_ok
  Abort
  gkplus_leave_ok:
FunctionEnd
