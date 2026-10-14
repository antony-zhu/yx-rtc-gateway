param(
    [string]$Compiler = 'C:/msys64/mingw64/bin/gcc.exe',
    [string]$IdfPath = 'C:/Espressif/frameworks/esp-idf-v5.5.3'
)
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testBinary = Join-Path ([System.IO.Path]::GetTempPath()) ('yx-gateway-protocol-' + [guid]::NewGuid().ToString('N') + '.exe')
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Compiler) + [System.IO.Path]::PathSeparator + $env:PATH
    $compilerArguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', (Join-Path $PSScriptRoot 'stubs'),
        '-I', (Join-Path $projectRoot 'embedded/components/gw_app/include'),
        '-I', (Join-Path $IdfPath 'components/json/cJSON'),
        (Join-Path $PSScriptRoot 'test_gateway_protocol.c'),
        (Join-Path $IdfPath 'components/json/cJSON/cJSON.c'),
        '-o', $testBinary
    )
    & $Compiler @compilerArguments
    if ($LASTEXITCODE -ne 0) { throw 'Gateway protocol test compilation failed.' }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { throw 'Gateway protocol tests failed.' }
} finally {
    $env:PATH = $originalPath
    if (Test-Path -LiteralPath $testBinary) { Remove-Item -LiteralPath $testBinary }
}
