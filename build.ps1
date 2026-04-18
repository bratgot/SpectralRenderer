# build.ps1 - build SpectralRenderer, show only real errors, log everything

$ErrorActionPreference = "Continue"
$buildLog = "build.log"
$errorPattern = 'error [A-Z]+\d+|: error:|fatal error|CMake Error'

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
} else {
    $summary = Select-String -Path $buildLog -Pattern '\d+ Error\(s\)' | Select-Object -Last 1
    Write-Host "BUILD FAILED (exit $exit)" -ForegroundColor Red
    if ($summary) { Write-Host "  $($summary.Line.Trim())" -ForegroundColor Red }
    Write-Host "Full log: $(Resolve-Path $buildLog)" -ForegroundColor Yellow
}

exit $exit