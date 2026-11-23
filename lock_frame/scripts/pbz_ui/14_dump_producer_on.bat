@echo off
rem ============================================================================
rem  14_dump_producer_on.bat - enable producer-side P/Hmask/U/Scaling dump.
rem
rem  UiSynthDump is read once when the producer creates its first synth frame, so
rem  the game MUST be closed before running this. After it writes the ini:
rem    1) start the game and load a save
rem    2) run 13_dump_patch.bat (consumer)
rem    3) producer writes <game>\\ui_synth_dump\\frame_00_p/h/u/s.bmp
rem    4) optionally run 14_dump_producer_off.bat after the session
rem
rem  frame_XX_p.bmp = DLSSG.Backbuffer (P, UI inside)
rem  frame_XX_h.bmp = HUDLessColor (mask reference, scene-referred)
rem  frame_XX_u.bmp = final synth/patch texture
rem  frame_XX_s.bmp = ScalingOutputColor (display-referred patch colour)
rem ============================================================================
call "%~dp0ui__env.bat"

tasklist /fi "imagename eq %GAME_NAME%" 2>nul | find /i "%GAME_NAME%" >nul
if not errorlevel 1 (
  echo [dump] close %GAME_NAME% first - UiSynthDump is read once at session start.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set_ini_dump.ps1" -Ini "%INI%" -UiSynthDump 3 -After 30
if errorlevel 1 ( pause & exit /b 1 )

echo.
echo [dump] producer dump enabled (3 frames, after 30 synth frames).
echo [dump] start the game, load a save, keep DLSS Frame Generation ON.
echo [dump] then run 13_dump_patch.bat in this folder.
echo [dump] producer output: %GAME_DIR%\\ui_synth_dump\\
echo [dump] after the session run 14_dump_producer_off.bat.
pause
