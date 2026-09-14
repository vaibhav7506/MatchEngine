param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Benchmark
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$originalPath = $env:PATH
Push-Location $projectRoot
try {
    # Prefer a system toolchain; fall back to the portable workspace tools.
    $portableCompiler = Join-Path $projectRoot '.tools\llvm-mingw-20260908-ucrt-x86_64\bin'
    $portableCMake = Join-Path $projectRoot '.tools\cmake-3.31.8-windows-x86_64\bin'
    if (Test-Path -LiteralPath $portableCMake) { $env:PATH = "$portableCMake;$env:PATH" }
    $buildDirectory = "build-$($Configuration.ToLower())"
    if (Test-Path -LiteralPath $portableCompiler) {
        $env:PATH = "$portableCompiler;$env:PATH"
        cmake -S . -B $buildDirectory -G 'MinGW Makefiles' "-DCMAKE_BUILD_TYPE=$Configuration" -DCMAKE_CXX_COMPILER=clang++
    } else {
        cmake -S . -B $buildDirectory "-DCMAKE_BUILD_TYPE=$Configuration"
    }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    cmake --build $buildDirectory --config $Configuration --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    if ($Benchmark) {
        $exe = Join-Path $buildDirectory 'matchengine_benchmark.exe'
        if (!(Test-Path -LiteralPath $exe)) { $exe = Join-Path $buildDirectory "$Configuration\matchengine_benchmark.exe" }
        & $exe --orders 1000000
        if ($LASTEXITCODE -ne 0) { throw 'Throughput benchmark failed' }
        foreach ($depth in @(1000, 10000, 100000)) {
            & $exe --memory $depth
            if ($LASTEXITCODE -ne 0) { throw 'Memory benchmark failed' }
        }
    }
} finally { $env:PATH = $originalPath; Pop-Location }
