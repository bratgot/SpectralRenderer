# build.ps1 - build SpectralRenderer, show only real errors, log everything

$ErrorActionPreference = "Continue"
$buildLog = "build.log"
$errorPattern = 'error [A-Z]+\d+|: error:|fatal error|CMake Error|LNK\d+'
$dllPath = "build\SpectralRender\SpectralRender.dll"

Write-Host "Building..." -ForegroundColor Cyan

cmake --build build --config Release --parallel 2>&1 |
    Tee-Object -FilePath $buildLog |
    ForEach-Object {
        $line = $_.ToString()
        if ($line -match $errorPattern) {
            Write-Host $line -ForegroundColor Red
        }
    }

$exit = $LASTEXITCODE
Write-Host ""
if ($exit -eq 0) {
    Write-Host "BUILD SUCCEEDED" -ForegroundColor Green
    # Did cmake actually compile/link anything, or was it a no-op?
    # Used below to distinguish a legitimate incremental no-op from a
    # DLL lock-induced silent failure.
    $didWork = $false
    if (Test-Path $buildLog) {
        if (Select-String -Path $buildLog -Pattern 'ClCompile:|Link:|CustomBuild:|Creating library' -Quiet) {
            $didWork = $true
        }
    }
    if (Test-Path $dllPath) {
        $dllMtime = (Get-Item $dllPath).LastWriteTime
        $dllAgeSec = [int]((Get-Date) - $dllMtime).TotalSeconds
        if ($dllAgeSec -lt 30) {
            Write-Host "DLL: $dllPath (fresh, ${dllAgeSec}s ago)" -ForegroundColor Green
        } elseif (-not $didWork) {
            Write-Host "DLL: $dllPath ($dllMtime, no build work needed)" -ForegroundColor Green
        } else {
            Write-Host "DLL: $dllPath ($dllMtime, ${dllAgeSec}s ago -- not refreshed by this build)" -ForegroundColor Yellow
            Write-Host "     Build did work but the DLL was not updated. Nuke likely has it locked." -ForegroundColor Yellow
        }
    } else {
        Write-Host "WARNING: DLL not found at $dllPath" -ForegroundColor Yellow
    }
} else {
    $summary = Select-String -Path $buildLog -Pattern '\d+ Error\(s\)' | Select-Object -Last 1
    Write-Host "BUILD FAILED (exit $exit)" -ForegroundColor Red
    if ($summary) { Write-Host "  $($summary.Line.Trim())" -ForegroundColor Red }
    Write-Host "Full log: $(Resolve-Path $buildLog)" -ForegroundColor Yellow
}

exit $exit