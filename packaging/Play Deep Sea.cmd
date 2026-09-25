@echo off
setlocal
cd /d "%~dp0"
rem Open the Deep Sea launcher (game picker + graphics/controller settings).
rem For direct launch / advanced flags use: powershell -File Launch-WindWaker.ps1
rem
rem Relaunch safety ("it doesn't begin again" fix):
rem   - a missing launcher is reported instead of silently doing nothing
rem   - a leftover DeepSea.exe / moderngekko-run.exe from THIS folder
rem     (a crashed session whose window died but whose process survived) is
rem     detected and reported before we pile a second instance on top of it -
rem     the runner's runtime singleton rejects a new start while one lives
rem   - any start failure prints an error and pauses instead of vanishing

if exist "%~dp0DeepSea.exe" goto :check_stale
echo.
echo ERROR: DeepSea.exe is missing from:
echo   %~dp0
echo The bundle looks incomplete. Re-extract the release, or use
echo   powershell -File Launch-WindWaker.ps1
echo for direct launch with full diagnostics.
echo.
pause
exit /b 1

:check_stale
rem Vulkan driver pipeline disk cache must stay DISABLED (a stale cache caused
rem black screens - see provenance.json); neutralize an inherited enable.
if "%MODERNGEKKO_VK_PIPELINE_CACHE%"=="1" set "MODERNGEKKO_VK_PIPELINE_CACHE=0"

rem Detect leftover launcher/runner processes started from THIS bundle folder.
rem Scoped by full image path - other installs are never touched, and
rem processes whose path cannot be read are skipped rather than guessed at.
set "MG_BUNDLE_DIR=%~dp0"
powershell -NoProfile -Command "$b = $env:MG_BUNDLE_DIR.TrimEnd('\') + [System.IO.Path]::DirectorySeparatorChar; $stale = @(Get-Process -Name 'DeepSea','moderngekko-run' -ErrorAction SilentlyContinue | Where-Object { try { $x = $_.Path } catch { $x = $null }; ($null -ne $x) -and $x.StartsWith($b, [System.StringComparison]::OrdinalIgnoreCase) }); if ($stale.Count -gt 0) { foreach ($p in $stale) { Write-Output ('  PID ' + $p.Id + '  ' + $p.ProcessName) }; exit 1 }"
if errorlevel 1 goto :stale_warn
goto :launch

:stale_warn
echo.
echo WARNING: The Wind Waker is ALREADY RUNNING from this folder - see the
echo PID list above. A previous session may have crashed and left those
echo processes behind with no visible window. While they are alive a new
echo session cannot begin.
echo.
echo Fix: close their windows, or end those processes in Task Manager,
echo then run this shortcut again.
echo.
choice /C YN /N /M "Try starting the launcher anyway? [Y/N] "
if errorlevel 2 exit /b 1

:launch
start "" "%~dp0DeepSea.exe" %*
if errorlevel 1 goto :fail
endlocal
exit /b 0

:fail
echo.
echo ERROR: failed to start DeepSea.exe.
echo Exit code: %errorlevel%
echo If antivirus quarantined or blocked it, restore or allow it, then try again.
echo.
pause
exit /b 1
