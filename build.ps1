param([string]$Compiler = 'F:\Software\TDM-GCC-64\bin\g++.exe', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!(Test-Path -LiteralPath $Compiler)) { $Compiler = (Get-Command g++ -ErrorAction Stop).Source }
    $resourceCompiler = Join-Path (Split-Path -Parent $Compiler) 'windres.exe'
    New-Item -ItemType Directory -Path 'dist' -Force | Out-Null
    & (Join-Path $PSScriptRoot 'packaging\Make-Assets.ps1')
    & $resourceCompiler app.rc -O coff -o dist/app-res.o
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    $warn = '-Wall', '-Wextra', '-Wno-missing-field-initializers'
    $net = '-lws2_32', '-lsecur32', '-lcrypt32', '-lbcrypt'
    & $Compiler '-std=c++17' -O3 -DNDEBUG @warn -municode -mwindows -static '-static-libgcc' '-static-libstdc++' -s main.cpp dist/app-res.o -o dist/TDM.exe @net '-lcomctl32' '-lole32' '-luuid' '-lshell32' '-lcomdlg32' '-lgdi32' '-luxtheme' '-ldwmapi'
    if ($LASTEXITCODE) { throw 'Application compilation failed' }
    if (!$SkipTests) {
        & $Compiler '-std=c++17' -O2 @warn -municode -static tests.cpp -o dist/engine-tests.exe @net '-lole32' '-luuid' '-lshell32'
        if ($LASTEXITCODE) { throw 'Engine test compilation failed' }
        & .\dist\engine-tests.exe
        if ($LASTEXITCODE) { throw 'Engine tests failed' }
        & $Compiler '-std=c++17' -O2 @warn -municode -static ui-tests.cpp -o dist/ui-tests.exe '-lws2_32' '-lshell32'
        if ($LASTEXITCODE) { throw 'UI test compilation failed' }
        & .\dist\ui-tests.exe (Join-Path $PSScriptRoot 'dist\TDM.exe')
        if ($LASTEXITCODE) { throw 'UI integration tests failed' }
    }
    Write-Host "Built: $PSScriptRoot\dist\TDM.exe"
} finally { Pop-Location }
