param(
    [Parameter(Mandatory=$true)][string]$Sdk,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$NdkVersion = '27.0.12077973',
    [string]$CmakeVersion = '3.22.1',
    [string]$Serial = 'emulator-5554'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$build = Join-Path $repo 'dsp-core/build/dg-stage'
$cmake = Join-Path $Sdk "cmake/$CmakeVersion/bin/cmake.exe"
$ninja = Join-Path $Sdk "cmake/$CmakeVersion/bin/ninja.exe"
$toolchain = Join-Path $Sdk "ndk/$NdkVersion/build/cmake/android.toolchain.cmake"
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$out = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    $null = Get-Command $Program -ErrorAction Stop
    $global:LASTEXITCODE = 0
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = & $Program @Arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($code -ne 0) { throw "$Program failed ($code): $lines" }
    return $lines
}
Invoke-Checked $cmake @('-S', (Join-Path $repo 'dsp-core'), '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", '-DANDROID_ABI=x86_64',
    '-DANDROID_PLATFORM=26', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release',
    '-DDREAMGUARD_DSP_BUILD_TESTS=ON', '-DDREAMGUARD_DSP_BUILD_TOOLS=ON') | Out-Null
Invoke-Checked $cmake @('--build', $build) | Out-Null
$remote = '/data/local/tmp/dg-stage'
Invoke-Checked $adb @('-s',$Serial,'shell','mkdir','-p',$remote) | Out-Null
$tests = @('dg_lifecycle_tests','dg_header_c_compat','dg_pcm_tests','dg_meter_tests',
    'dg_lufs_tests','dg_compressor_tests','dg_limiter_tests','dg_true_peak_tests','dg_pcm16_processor_tests')
foreach ($test in $tests) {
    Invoke-Checked $adb @('-s',$Serial,'push',(Join-Path $build "tests/$test"),"$remote/$test") | Out-Null
    Invoke-Checked $adb @('-s',$Serial,'shell','chmod','755',"$remote/$test") | Out-Null
    $command = if ($test -eq 'dg_limiter_tests') { "DG_TRUE_PEAK_EXPORT=$remote $remote/$test" } else { "$remote/$test" }
    Invoke-Checked $adb @('-s',$Serial,'shell',$command) | Set-Content -Encoding utf8 (Join-Path $out "$test.txt")
}
Invoke-Checked $adb @('-s',$Serial,'push',(Join-Path $build 'tools/bench/dg_dsp_bench'),"$remote/dg_dsp_bench") | Out-Null
Invoke-Checked $adb @('-s',$Serial,'shell','chmod','755',"$remote/dg_dsp_bench") | Out-Null
# Redirect on Android so benchmark stderr metadata stays outside the CSV.
Invoke-Checked $adb @('-s',$Serial,'shell',"$remote/dg_dsp_bench > $remote/bench.csv 2> $remote/bench-meta.txt") | Out-Null
foreach ($name in @('bench.csv','bench-meta.txt')) {
    Invoke-Checked $adb @('-s',$Serial,'pull',"$remote/$name",(Join-Path $out $name)) | Out-Null
}
$fixtures = Join-Path $build 'fixtures'
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null
$fixturePaths = Invoke-Checked $adb @('-s',$Serial,'shell',"ls $remote/tp_*.f32")
foreach ($path in $fixturePaths) {
    $name = [IO.Path]::GetFileName($path.Trim())
    if ($name -notmatch '^tp_\d+_[12]_[01]_[0-5]\.f32$') { throw "Unexpected fixture: $path" }
    Invoke-Checked $adb @('-s',$Serial,'pull',$path.Trim(),(Join-Path $fixtures $name)) | Out-Null
}
$metadata = @("UTC=$([DateTime]::UtcNow.ToString('o'))", "NDK=$NdkVersion", "ABI=x86_64 API=26 Release c++_static", "Serial=$Serial")
$metadata += Invoke-Checked $adb @('-s',$Serial,'shell','getprop','ro.build.fingerprint')
$metadata += Invoke-Checked $adb @('-s',$Serial,'shell','uname','-a')
$metadata += "source_commit=$(git -C $repo rev-parse HEAD)"
$metadata += "source_dirty=$(git -C $repo diff --stat)"
$metadata | Set-Content -Encoding utf8 (Join-Path $out 'environment.txt')
$sourceFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $repo 'dsp-core/src') -File
    Get-ChildItem -LiteralPath (Join-Path $repo 'dsp-core/include/dreamguard') -File
    Get-ChildItem -LiteralPath (Join-Path $repo 'dsp-core/tests') -File
    Get-ChildItem -LiteralPath $PSScriptRoot -File
)
$sourceFiles | Sort-Object FullName | ForEach-Object {
    [PSCustomObject]@{path=$_.FullName.Substring($repo.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
} | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $out 'source-sha256.csv')
Write-Output "Passed $($tests.Count) test executables; benchmark CSV: $out; fixtures: $build/fixtures"
