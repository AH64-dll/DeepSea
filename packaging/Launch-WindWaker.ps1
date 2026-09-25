param(
    [string]$GameRoot,
    [switch]$ChooseGame,    # force the folder picker even if a saved path exists
    [switch]$ValidateOnly,  # validate + save the path, don't launch
    [switch]$KillStaleRunners  # end leftover moderngekko-run.exe from a crashed
                               # session without prompting (for scripted callers)
)
$ErrorActionPreference = 'Stop'

function ConvertTo-QuotedArg([string]$arg) {
    # Windows C-runtime rule: a backslash run immediately before a closing
    # quote must be doubled, else \" is consumed as an escaped quote and the
    # argument boundary collapses. Drive-root paths ('D:\') hit this.
    '"' + ($arg -replace '(\\+)$', '$1$1') + '"'
}

function Test-GameRoot([string]$root, [ref]$reason) {
    # $reason gets a user-facing explanation when this returns $false.
    $reason.Value = ''
    if ([string]::IsNullOrWhiteSpace($root)) {
        $reason.Value = 'No folder selected.'
        return $false
    }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        $reason.Value = "Folder does not exist or is not accessible:`n$root"
        return $false
    }
    foreach ($part in @('sys\main.dol', 'sys\boot.bin')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $part) -PathType Leaf)) {
            $reason.Value = "Game folder is missing $part.`nSelect an extracted disc folder (sys + files), not an ISO/RVZ file or its parent directory."
            return $false
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'files') -PathType Container)) {
        $reason.Value = "Game folder is missing the files directory.`nThe dump looks incomplete - re-extract the full disc contents."
        return $false
    }
    try {
        $boot = [System.IO.File]::ReadAllBytes((Join-Path $root 'sys\boot.bin'))
    } catch {
        $reason.Value = "Cannot read sys\boot.bin - check folder permissions.`n$($_.Exception.Message)"
        return $false
    }
    if ($boot.Length -lt 6 -or [System.Text.Encoding]::ASCII.GetString($boot, 0, 6) -ne 'GZLE01') {
        $reason.Value = 'This release requires the USA disc (GZLE01).`nEuropean (GZLP01) and Japanese (GZLJ01) dumps are not supported by the recompiled module.'
        return $false
    }
    return $true
}

function Read-SavedGamePath([string]$file) {
    # default-game.txt is UTF-8 without BOM (this script and the runner's
    # WriteSavedGameRoot both write it that way). PS 5.1's Get-Content decodes
    # BOM-less files with the ANSI codepage, which mangles non-ASCII paths —
    # pin -Encoding UTF8 so a unicode game path round-trips instead of
    # reprompting the user on every launch.
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $null }
    $line = Get-Content -LiteralPath $file -Encoding UTF8 -TotalCount 1
    if ($null -eq $line) { return $null }
    $line = $line.Trim()
    if ($line.Length -eq 0) { return $null }
    return $line
}

function Save-GamePath([string]$file, [string]$root) {
    # UTF-8 without BOM via File.WriteAllText's default — the same bytes the
    # runner's WriteSavedGameRoot produces, so either side can read the other.
    $dir = [System.IO.Path]::GetDirectoryName($file)
    if ($dir -and -not (Test-Path -LiteralPath $dir -PathType Container)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($file, $root + [Environment]::NewLine)
}

function Get-StaleBundleProcesses {
    # Live game-runner processes launched from THIS bundle folder only - a
    # crashed session can leave moderngekko-run.exe alive after its window is
    # gone ("it doesn't begin again"), and the runner's runtime singleton
    # ("only one ModernGekko runtime may be active per process" /
    # "runtime is already running") then rejects every new start. Scoped by
    # full image path under the bundle root so a copy from another install is
    # never touched; processes whose path cannot be read are skipped, never
    # guessed at.
    param([string]$Name, [string]$BundleRoot)
    $prefix = $BundleRoot.TrimEnd('\') + [System.IO.Path]::DirectorySeparatorChar
    $found = @()
    foreach ($p in (Get-Process -Name $Name -ErrorAction SilentlyContinue)) {
        $image = $null
        try { $image = $p.Path } catch { }
        if ($image -and $image.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $found += $p
        }
    }
    return $found
}

try {
    Set-Location -LiteralPath $PSScriptRoot
    # Whether -GameRoot was explicitly passed AND used (vs picker/saved-path
    # flow): controls the strict --game switch below. -ChooseGame discards a
    # passed -GameRoot in favour of the picker, so the pick is treated as
    # non-strict. Must be captured before the picker loop overwrites $GameRoot.
    $explicitGameRoot = $PSBoundParameters.ContainsKey('GameRoot') -and -not $ChooseGame
    $runner = Join-Path $PSScriptRoot 'moderngekko-run.exe'
    $module = Join-Path $PSScriptRoot 'gGZLE01_recomp.dll'
    $mods   = Join-Path $PSScriptRoot 'Mods'
    $userDir = Join-Path $PSScriptRoot 'assets\user-dir'
    $defaultGame = Join-Path $userDir 'default-game.txt'
    # A bundle either carries the game module or the setup kit that builds it
    # from the player's own disc (disc-builder\) on first launch.
    $builder = Join-Path $PSScriptRoot 'disc-builder\build.py'
    $builderPython = Join-Path $PSScriptRoot 'disc-builder\python\python.exe'
    $bundledModule = Test-Path -LiteralPath $module -PathType Leaf
    $requiredFiles = @($runner, (Join-Path $PSScriptRoot 'libwinpthread-1.dll'), (Join-Path $mods 'frame60-accum.mgm\mod.dll'))
    if ($bundledModule) { $requiredFiles += $module } else { $requiredFiles += @($builder, $builderPython) }
    foreach ($required in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing required file: $required" }
    }

    # Resolve a candidate root: explicit param > saved path.
    $candidate = $null
    if (-not $ChooseGame) {
        if ($GameRoot) {
            $candidate = $GameRoot
        } elseif (Test-Path -LiteralPath $defaultGame -PathType Leaf) {
            $saved = Read-SavedGamePath $defaultGame
            if ($saved) {
                $candidate = $saved
                $reason = ''
                if (-not (Test-GameRoot $saved ([ref]$reason))) {
                    Write-Host "Saved game path is no longer valid: $reason" -ForegroundColor Yellow
                    Write-Host 'Opening the folder picker so you can locate it again...'
                    $candidate = $null
                }
            }
        }
    }

    # Validate the candidate; if it fails, or none exists, run the picker loop.
    $reason = ''
    $valid = $candidate -and (Test-GameRoot $candidate ([ref]$reason))
    if (-not $valid -and $ValidateOnly -and -not $ChooseGame) {
        # Scripted validation must never block on a GUI picker — report and
        # exit non-zero. (-ChooseGame explicitly asks for the picker, so
        # -ChooseGame -ValidateOnly still picks-then-validates.)
        if ($candidate) { Write-Host "Invalid game folder '$candidate': $reason" -ForegroundColor Red }
        else { Write-Host 'No game folder configured (none saved, none passed).' -ForegroundColor Red }
        exit 1
    }
    while (-not $valid) {
        if ($candidate -and $reason) {
            Write-Host "Invalid game folder: $reason" -ForegroundColor Yellow
        }
        # WinForms is only needed when a human must pick a folder — a valid
        # saved/param path must never depend on the assembly loading.
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.FolderBrowserDialog
        $picker.Description = 'Choose the extracted USA Wind Waker folder containing sys and files.'
        $picker.ShowNewFolderButton = $false
        $picked = $picker.ShowDialog() -eq 'OK'
        $GameRoot = $picker.SelectedPath
        $picker.Dispose()
        if (-not $picked) { throw 'No game folder selected. The port needs your legally obtained, extracted USA Wind Waker disc folder to run.' }
        $candidate = $GameRoot
        if (Test-GameRoot $candidate ([ref]$reason)) { $valid = $true; break }
        $answer = [System.Windows.Forms.MessageBox]::Show(
            "$reason`n`nLocate the game folder again?",
            'Wind Waker - invalid game folder',
            [System.Windows.Forms.MessageBoxButtons]::RetryCancel,
            [System.Windows.Forms.MessageBoxIcon]::Warning)
        if ($answer -ne 'Retry') { throw "Setup cancelled: $reason" }
        $candidate = $null
    }
    # .ProviderPath, not .Path: on Windows PowerShell 5.1, .Path returns the
    # provider-qualified 'Microsoft.PowerShell.Core\FileSystem::\\server\share'
    # form for UNC picks — saved verbatim into default-game.txt it would
    # permanently break a network-share game folder. .ProviderPath is always
    # the filesystem-native spelling.
    $GameRoot = (Resolve-Path -LiteralPath $candidate).ProviderPath

    New-Item -ItemType Directory -Path $userDir -Force | Out-Null
    Save-GamePath $defaultGame $GameRoot
    Write-Host "Game path saved: $GameRoot"

    if ($ValidateOnly) {
        Write-Host 'Validation OK. Run "Play Deep Sea.cmd" to start the game.'
        exit 0
    }

    if (-not $bundledModule) {
        # First launch translates and compiles the game code from the disc
        # above (about 10 minutes on a typical PC); the result is cached under the
        # user dir and later launches reuse it after a quick check.
        Write-Host 'Game setup: building the game from your disc. The first time takes a while; later launches start right away.'
        $modulesDir = Join-Path $userDir 'modules'
        & $builderPython -I $builder $GameRoot --output $modulesDir | ForEach-Object {
            $state = $null
            try { $state = $_ | ConvertFrom-Json } catch { }
            if ($state -and $state.stage) {
                if ($state.total -gt 1) {
                    Write-Progress -Activity 'Game setup' -Status $state.stage -PercentComplete ([int](100 * $state.completed / $state.total))
                } else {
                    Write-Host $state.stage
                }
                if ($state.error) { Write-Host $state.error -ForegroundColor Red }
            }
        }
        $setupExit = $LASTEXITCODE
        Write-Progress -Activity 'Game setup' -Completed
        if ($setupExit -ne 0) { throw 'Game setup failed; see the message above. Logs are kept in the modules folder.' }
        $module = (Get-Content -LiteralPath (Join-Path $modulesDir 'GZLE01\active-module.txt') -TotalCount 1 -Encoding UTF8).Trim()
        if (-not (Test-Path -LiteralPath $module -PathType Leaf)) { throw "Game setup did not produce the game module: $module" }
    }

    $logs = Join-Path $PSScriptRoot 'logs'
    New-Item -ItemType Directory -Path $logs -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $stdout = Join-Path $logs "$stamp.stdout.log"
    $stderr = Join-Path $logs "$stamp.stderr.log"
    Write-Host 'Starting The Wind Waker...'
    Write-Host 'The runner verifies all game assets before opening the window. Please wait.'
    Write-Host "Game: $GameRoot"
    Write-Host "Logs: $logs"
    # 60fps stack (verified combination; same envs used in all release-candidate benches)
    $env:MODERNGEKKO_FRAME60_ACCUM = '1'
    $env:MODERNGEKKO_J3D_INTERP = '1'
    $env:MODERNGEKKO_RFRAME_RENDER = '1'
    $env:STATICRECOMP_IDLE_PCS = '80307ef4@-0x66b0'
    # The Vulkan driver pipeline DISK cache must stay DISABLED: a stale
    # Vulkan-Pipeline-*.cache blob seeded black-screen-causing pipeline
    # entries across boots (see provenance.json). The runner defaults it off
    # and MODERNGEKKO_VK_PIPELINE_CACHE=1 re-enables it - never enable it
    # here, and neutralize a value inherited from the calling shell. Unset
    # or '0' is left exactly as-is (we do not create the variable).
    if ($env:MODERNGEKKO_VK_PIPELINE_CACHE -and $env:MODERNGEKKO_VK_PIPELINE_CACHE -ne '0') {
        $env:MODERNGEKKO_VK_PIPELINE_CACHE = '0'
        Write-Host 'Vulkan pipeline disk cache enable inherited from the environment was overridden back to disabled (0).'
    }
    # --game only for an explicit -GameRoot argument: it puts the runner in
    # strict non-interactive mode, which is right for scripted callers. For a
    # picked or saved path the runner reads <user-dir>\default-game.txt itself,
    # so a folder that passes our light checks but fails the runner's deeper
    # probe/inspect (incomplete dump, corrupt main.dol, empty files\) gets the
    # runner's own locate/retry/cancel recovery instead of a strict exit 2 —
    # under --game the just-saved path would fail the same way on every
    # relaunch, dead-ending the user.
    # --present-mode fifo: present defaults to immediate, which tear-strobes
    # ("flickerups") when the 59.9 Hz VI meets a 60 Hz panel; an explicit
    # --present-mode always wins. GFX.ini pins the same mode for runs the
    # launcher spawns itself.
    $argumentList = @(
        '--module', (ConvertTo-QuotedArg $module),
        '--user-dir', (ConvertTo-QuotedArg $userDir),
        '--mods', (ConvertTo-QuotedArg $mods),
        '--present-mode', 'fifo')
    if ($explicitGameRoot) {
        $argumentList = @('--game', (ConvertTo-QuotedArg $GameRoot)) + $argumentList
    }
    $arguments = $argumentList -join ' '
    # Relaunch robustness: a crashed session can leave moderngekko-run.exe
    # alive after its window closed (observed 2026-09-22: a runner whose log
    # went silent at 13:37 was still alive until it crashed at 14:17 -
    # logs\crash-*.txt), and the runner's runtime singleton rejects a new
    # start while that leftover lives ("runtime is already running"), which
    # is exactly "it doesn't begin again". Detect leftovers under this
    # bundle's path and offer to end them before starting a new session.
    $stale = @(Get-StaleBundleProcesses 'moderngekko-run' $PSScriptRoot)
    if ($stale.Count -gt 0) {
        Write-Host 'A leftover game runner from a previous session is still running:' -ForegroundColor Yellow
        foreach ($p in $stale) {
            $when = ''
            try { $when = ', started ' + $p.StartTime } catch { }
            Write-Host ("  PID {0} ({1}{2})" -f $p.Id, $p.ProcessName, $when) -ForegroundColor Yellow
        }
        $doKill = $false
        if ($KillStaleRunners) {
            $doKill = $true
        } else {
            $answer = Read-Host 'End the leftover runner before starting? [Y]es / [N]o, keep it / [A]bort launch'
            $answer = "$answer".Trim().ToLowerInvariant()
            if ($answer -eq 'a') {
                throw 'Launch aborted: a leftover game runner from a previous session is still active. End it in Task Manager (or rerun with -KillStaleRunners), then start again.'
            }
            $doKill = ($answer -eq 'y')
        }
        if ($doKill) {
            foreach ($p in $stale) {
                try {
                    Stop-Process -Id $p.Id -Force -ErrorAction Stop
                    Write-Host "Ended leftover runner (PID $($p.Id))."
                } catch {
                    Write-Host "Could not end leftover runner (PID $($p.Id)): $($_.Exception.Message)" -ForegroundColor Yellow
                }
            }
            # Brief pause so Windows can release the dead runner's handles on
            # the game folder / user-dir before the new instance grabs them.
            Start-Sleep -Milliseconds 500
        } else {
            Write-Host 'Continuing with the leftover runner still alive. If the game does not begin, end it in Task Manager and start again.' -ForegroundColor Yellow
        }
    }
    $startedAt = Get-Date
    try {
        $gameProcess = Start-Process -FilePath $runner -ArgumentList $arguments -WorkingDirectory $PSScriptRoot -WindowStyle Normal -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -ErrorAction Stop
    } catch {
        throw ("Failed to start the game runner:`n{0}`n{1}`nCheck that the file is not blocked or quarantined by antivirus and that the logs folder is writable." -f $runner, $_.Exception.Message)
    }
    # PS 5.1: with redirected streams, ExitCode stays $null after WaitForExit
    # unless the process handle is materialized first (verified: a clean exit
    # otherwise throws "Game exited with code ." on every run).
    $null = $gameProcess.Handle
    $gameProcess.WaitForExit()
    $exitCode = $null
    try { $exitCode = $gameProcess.ExitCode } catch {}
    if ($exitCode -ne 0) {
        Get-Content -LiteralPath $stderr -Tail 35
        # The runner's last-chance crash reporter writes logs\crash-*.dmp/.txt
        # on a hard failure - point at anything THIS run left behind.
        $crashReports = @(Get-ChildItem -LiteralPath $logs -Filter 'crash-*.txt' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $startedAt })
        foreach ($c in $crashReports) {
            Write-Host "Crash report for this run: $($c.FullName)" -ForegroundColor Yellow
        }
        if ($exitCode -eq 2) {
            throw 'Setup was cancelled or did not finish (exit 2). Run "Play Deep Sea.cmd" again to retry, or run this script with -ChooseGame to pick a different folder.'
        }
        if ($null -eq $exitCode) { $exitCode = '(unknown)' }
        throw "Game exited with code $exitCode. See $stderr"
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
