# The vertex-conversion differential test: src/VertexFormat.cpp against itself at a git revision.
#
#   .\utils\vfdiff\run.ps1              # against HEAD
#   .\utils\vfdiff\run.ps1 -Ref HEAD~3  # against any revision
#   .\utils\vfdiff\run.ps1 -SelfTest    # ... and prove the harness can fail
#
# Exit code 0 means every case matched. See README.md.

[CmdletBinding()]
param(
    [string]$Ref = 'HEAD',
    [string]$Compiler = 'C:\Program Files\LLVM\bin\clang++.exe',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("vfdiff-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

if (-not (Test-Path $Compiler)) {
    throw "no compiler at $Compiler - pass -Compiler with the path to clang++/g++"
}

Copy-Item (Join-Path $repo 'src\VertexFormat.h')   (Join-Path $work 'VertexFormat.h')
Copy-Item (Join-Path $repo 'src\VertexFormat.cpp') (Join-Path $work 'VertexFormat.cpp')
Copy-Item (Join-Path $PSScriptRoot 'main.cpp')     (Join-Path $work 'main.cpp')

# The reference half, re-namespaced into `refvulkan` so both link into one binary.
Push-Location $repo
foreach ($pair in @(@('src/VertexFormat.h', 'RefVertexFormat.h'), @('src/VertexFormat.cpp', 'RefVertexFormat.cpp'))) {
    $text = (git show "${Ref}:$($pair[0])") -join "`n"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "git show ${Ref}:$($pair[0]) failed" }
    $text = $text.Replace('namespace vulkan {', 'namespace refvulkan {')
    $text = $text.Replace('} // namespace vulkan', '} // namespace refvulkan')
    $text = $text.Replace('#include "VertexFormat.h"', '#include "RefVertexFormat.h"')
    Set-Content -Path (Join-Path $work $pair[1]) -Value $text -Encoding utf8 -NoNewline
}
Pop-Location

Push-Location $work
try {
    & $Compiler -std=c++20 -O2 -o vfdiff.exe main.cpp VertexFormat.cpp RefVertexFormat.cpp
    if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
    Write-Host "current vs ${Ref}:" -NoNewline
    .\vfdiff.exe
    $result = $LASTEXITCODE

    if ($SelfTest) {
        # A harness that cannot fail proves nothing. Perturb one component and require a report.
        $broken = (Get-Content VertexFormat.cpp -Raw).Replace(
            '      v.pos[3] = ReadFloat(p + 12);',
            '      v.pos[3] = ReadFloat(p + 12) * 1.0001f;')
        Set-Content -Path VertexFormat.cpp -Value $broken -NoNewline
        & $Compiler -std=c++20 -O2 -o vfbroken.exe main.cpp VertexFormat.cpp RefVertexFormat.cpp
        .\vfbroken.exe | Select-Object -Last 1 | ForEach-Object { Write-Host "self-test (must fail): $_" }
        if ($LASTEXITCODE -eq 0) { throw 'SELF-TEST DID NOT FAIL - the harness proves nothing' }
        Write-Host 'self-test reported the deliberate break, as it must.'
    }
} finally {
    Pop-Location
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

exit $result
