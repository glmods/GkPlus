# Which draw painted the pixel at (X, Y)? A binary search over render.draw_hide.
#
# Hiding a WINDOW rather than truncating a prefix, for the reason notes 4.29 gives: a prefix
# truncates the depth and stencil buffers too, so a draw that merely becomes unoccluded reads as
# the draw that painted the pixel. Hiding leaves the rest of the frame intact.
#
# 900 ms between setting the range and capturing, also from 4.29 - at 300 ms the shot lags one
# step behind and the search converges neatly on the wrong answer.

. "$PSScriptRoot\shot-gunlok.ps1"

function Get-Pixel([string]$Path, [int]$X, [int]$Y) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $Path
    $c = $bmp.GetPixel($X, $Y)
    $bmp.Dispose()
    return "$($c.R),$($c.G),$($c.B)"
}

function Find-Draw {
    param([int]$X, [int]$Y, [int]$Count, [int]$Port = $global:GunlokReplPort)

    Repl 'render.draw_hide = [1, 0]; 1' $Port | Out-Null
    Start-Sleep -Milliseconds 900
    Get-GunlokShot "$PSScriptRoot\_probe_base.png" | Out-Null
    $base = Get-Pixel "$PSScriptRoot\_probe_base.png" $X $Y
    Write-Host "  baseline pixel = $base"

    # Invariant: hiding [lo, hi] changes the pixel, so the draw that painted it is in there.
    $lo = 0; $hi = $Count - 1
    Repl "render.draw_hide = [$lo, $hi]; 1" $Port | Out-Null
    Start-Sleep -Milliseconds 900
    Get-GunlokShot "$PSScriptRoot\_probe_all.png" | Out-Null
    if ((Get-Pixel "$PSScriptRoot\_probe_all.png" $X $Y) -eq $base) {
        Repl 'render.draw_hide = [1, 0]; 1' $Port | Out-Null
        throw "hiding every draw leaves that pixel unchanged - nothing in the list paints it"
    }

    while ($lo -lt $hi) {
        $mid = [math]::Floor(($lo + $hi) / 2)
        Repl "render.draw_hide = [$lo, $mid]; 1" $Port | Out-Null
        Start-Sleep -Milliseconds 900
        Get-GunlokShot "$PSScriptRoot\_probe.png" | Out-Null
        if ((Get-Pixel "$PSScriptRoot\_probe.png" $X $Y) -ne $base) {
            $hi = $mid           # the culprit is in the half just hidden
        } else {
            $lo = $mid + 1       # ... otherwise it is in the other half
        }
        Write-Host "  window now [$lo, $hi]"
    }
    Repl 'render.draw_hide = [1, 0]; 1' $Port | Out-Null
    return $lo
}
