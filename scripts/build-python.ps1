param([switch]$InstallDependencies)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$originalPath = $env:PATH
$originalGenerator = $env:CMAKE_GENERATOR
$originalCompiler = $env:CXX
Push-Location $projectRoot
try {
    if (!(Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
        python -m venv .venv
        if ($LASTEXITCODE -ne 0) { throw 'Virtual environment creation failed' }
    }
    if ($InstallDependencies) {
        & .\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
        if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
    }
    $compiler = Join-Path $projectRoot '.tools\llvm-mingw-20260908-ucrt-x86_64\bin'
    $cmake = Join-Path $projectRoot '.tools\cmake-3.31.8-windows-x86_64\bin'
    if (Test-Path -LiteralPath $cmake) { $env:PATH = "$cmake;$env:PATH" }
    if (Test-Path -LiteralPath $compiler) {
        $env:PATH = "$compiler;$env:PATH"
        $env:CMAKE_GENERATOR = 'MinGW Makefiles'
        $env:CXX = 'clang++'
    }
    & .\.venv\Scripts\python.exe -m pip install . --no-build-isolation --no-deps
    if ($LASTEXITCODE -ne 0) { throw 'Python wheel build/install failed' }
    & .\.venv\Scripts\python.exe -m pytest
    if ($LASTEXITCODE -ne 0) { throw 'Python tests failed' }
} finally {
    $env:PATH = $originalPath
    $env:CMAKE_GENERATOR = $originalGenerator
    $env:CXX = $originalCompiler
    Pop-Location
}
