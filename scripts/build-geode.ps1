param(
    [switch]$SkipBackend
)

$ErrorActionPreference = "Stop"

$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$GeodeDirectory = Join-Path $RepositoryRoot "geode"
$ResourcesDirectory = Join-Path $GeodeDirectory "resources"
$BackendExecutable = Join-Path $ResourcesDirectory "gd-ai-backend.exe"

Set-Location $RepositoryRoot

if (-not $SkipBackend) {
    Write-Host "Building bundled Python backend..."

    python -m pip install -U pip
    python -m pip install -e .
    python -m pip install "pyinstaller>=6,<7" tbb

    python -m PyInstaller --noconfirm --clean --onefile --name gd-ai-backend --collect-all librosa --collect-all soundfile --collect-all numba --collect-all llvmlite --distpath "$RepositoryRoot\backend-dist" --workpath "$RepositoryRoot\backend-build" --specpath "$RepositoryRoot\backend-build" "$RepositoryRoot\backend_entry.py"

    New-Item -ItemType Directory -Force -Path $ResourcesDirectory | Out-Null
    Copy-Item "$RepositoryRoot\backend-dist\gd-ai-backend.exe" $BackendExecutable -Force
}

if (-not (Test-Path $BackendExecutable)) {
    Write-Warning "gd-ai-backend.exe is missing. The mod will build, but in-game analysis will report that the backend is unavailable."
}

Write-Host "Building Geode mod..."
Set-Location $GeodeDirectory
geode build --config Release

Write-Host ""
Write-Host "Done. Look for duckydev.gd-ai-editor.geode in geode\build or geode\build\Release."
