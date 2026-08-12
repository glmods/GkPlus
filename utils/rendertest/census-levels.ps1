# `render.normal_census` across every shipped single-player level.
#
# The census is per FRAME - it walks the draws the last recorded frame issued - so a reading is one
# camera's worth of a level and not the level's whole mesh. That is the honest limitation and the
# reason this settles the camera first: the settled shot is at least a *reproducible* sample, which
# a reading taken at an arbitrary moment of a cutscene is not.
#
# One launch per level rather than `levels.start` in a loop. Slower, and it is what keeps a level
# that fails to load from poisoning the fifteen after it - and `railway` is a live example of a
# level that takes gl.exe down inside its own ConvertParsedObjects (notes §4.60), so it is not in
# the list at all.
#
#     . .\utils\rendertest\census-levels.ps1
#     Measure-Census -Out census
#
# Writes `<Out>\<level>.txt` per level plus `<Out>\_failures.txt`, for a parser to read.

. "$PSScriptRoot\shoot-settled.ps1"

# The twelve numbered campaign levels plus the four §4.60 walked. `railway` is deliberately absent.
$CensusLevels = @(
    "level01", "level02", "level03", "level04", "level05", "level06",
    "level07", "level09", "level10", "level11", "level12", "level15",
    "prison", "junkyard", "cityruins", "Training_Level"
)

function Measure-Census {
    param(
        [string]$Out = "census",
        [string[]]$Levels = $CensusLevels,
        [int]$Port = 0   # 0 = a fresh ephemeral port per launch; $live is the one bound
    )
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $failures = @()

    foreach ($level in $Levels) {
        Write-Host "=== $level ===" -ForegroundColor Cyan
        try {
            Start-Gunlok -Renderer vulkan -Port $Port | Out-Null
            # Each launch binds its own port, so this has to be re-read per level.
            $live = $global:GunlokReplPort
            Repl "levels.start({script: `"$level.gls`", console: `"$level.gcs`"})" $live | Out-Null

            # Five of the fifteen campaign levels play no cutscene; the rest do, and a cutscene
            # camera never comes to rest. Both waits are therefore allowed to time out - a reading
            # taken mid-cutscene is still a reading of that level's geometry, and saying so beats
            # dropping eleven levels for tidiness.
            try { Dismiss-Briefing -TimeoutSeconds 90 | Out-Null } catch { Write-Host "  (no briefing)" }
            try { Wait-World -TimeoutSeconds 90 | Out-Null } catch { Write-Host "  (world wait timed out)" }
            try { Wait-CameraRest -TimeoutSeconds 60 | Out-Null } catch { Write-Host "  (camera never settled - cutscene?)" }

            $actors = ((Repl 'actors.count' $live) | ConvertFrom-Json).value
            $census = ((Repl 'render.normal_census()' $live) | ConvertFrom-Json).value
            if (-not $census) { throw "the census returned nothing" }
            # ConvertFrom-Json already unescaped the JSON string; the REPL's own \n survive as two
            # characters, so they are turned back into real newlines for the parser.
            $text = ($census -replace '\\n', "`n")
            Set-Content -Path (Join-Path $Out "$level.txt") -Value "actors: $actors`n$text"
            Write-Host "  ok, $actors actors"
        }
        catch {
            Write-Host "  FAILED: $_" -ForegroundColor Red
            $failures += "$level : $_"
        }
        finally {
            Stop-Process -Name gl, WerFault -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
        }
    }

    Set-Content -Path (Join-Path $Out "_failures.txt") -Value ($failures -join "`n")
    Write-Host "done: $($Levels.Count - $failures.Count)/$($Levels.Count) levels" -ForegroundColor Green
}
