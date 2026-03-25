@ECHO OFF
REM FusionOS AUTOEXEC.BAT
REM Executed automatically when COMMAND.COM starts an interactive session.
REM Equivalent to the classic MS-DOS AUTOEXEC.BAT.

REM ── Environment setup ─────────────────────────────────────────────────
SET OS=FusionOS
SET FUSIONOS=1
SET TEMP=C:\TEMP
SET TMP=C:\TEMP

REM ── PATH: DOS-style search path ───────────────────────────────────────
SET PATH=C:\BIN;C:\DOS;C:\GAMES;C:\UTILS;/usr/bin;/bin;/usr/local/bin

REM ── Custom prompt: drive + path + > ──────────────────────────────────
PROMPT $P$G

REM ── Greeting banner ──────────────────────────────────────────────────
ECHO.
ECHO  FusionOS -- MS-DOS Evolved -- 2025 Edition
ECHO  Type HELP for commands.  Type VER for version.
ECHO.

REM ── Create TEMP directory if it doesn't exist ─────────────────────────
IF NOT EXIST C:\TEMP MD C:\TEMP

REM ── Load optional user customisations ────────────────────────────────
IF EXIST C:\DOSUSER.BAT CALL C:\DOSUSER.BAT
