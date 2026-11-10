<#
.SYNOPSIS
    DG-012: compare this workspace's LUFS analyser against FFmpeg's ebur128 filter
    over identical, byte-for-byte synthetic PCM.

.DESCRIPTION
    Three moving parts, kept deliberately separate so that a missing tool can never
    look like a passing comparison:

      dg_lufs_compare --wav-dir --csv   writes the synthetic WAVs and our readings
      ffmpeg -af ebur128                measures the same WAV bytes independently
      this script                       pairs the two, reports the deltas, decides

    Two input modes:

      native (default)   run dg_lufs_compare here, then compare what it produced.
      pre-generated      -Csv <ours.csv> [-WavDir <dir>] : the CSV and the WAVs were
                         produced elsewhere (for example by the Android binary on an
                         emulator, then `adb pull`ed) and are used as-is.

    In both modes the reference numbers come from running ffmpeg on the actual WAV
    files. This script never edits the analyser, never widens the tolerance, never
    synthesises a value it did not read, and never deletes the input files it was
    given. If it cannot run, it exits non-zero and prints the exact commands to run
    by hand.

    Reference reading (high precision path)
    ---------------------------------------
    The summary's `I:` line is printed with one decimal, so it can only support a
    0.1 LU verdict. This script instead runs

        ebur128=metadata=1:peak=true,ametadata=mode=print:file=<temp>.txt

    and reads the last `lavfi.r128.I` value, which FFmpeg formats with three
    decimals. The one-decimal summary is still parsed, but only as a consistency
    witness: if metadata and summary differ by more than half a summary step, the
    case is a FAIL. FFmpeg accumulates integrated loudness in a 0.01 LU histogram,
    so three printed decimals are a text format, not 0.001 LU accuracy.

    Digital silence and streams shorter than one 400 ms block are not numeric
    cases. FFmpeg prints `-70.000` for them because `integrated_loudness` is
    initialised to `ABS_THRES` (-70) and `lavfi.r128.I` is emitted unconditionally;
    the same text can also be a genuine -70 LUFS measurement. The script therefore
    demands a second witness from the same metadata: every `lavfi.r128.M` block
    must be strictly below -70.000. Such cases are reported as `NO-DATA` and are
    never counted as passing. A `-70.000` with any M at or above -70 is a FAIL.
    This is a comparison with FFmpeg's implementation, not EBU/ITU certification.

.PARAMETER Tool
    Native mode only: path to the dg_lufs_compare executable. Defaults to searching
    the usual CMake build directories under dsp-core/build.

.PARAMETER Csv
    Pre-generated mode: the `ours.csv` written by `dg_lufs_compare --csv`. Selecting
    this mode makes the script read-only with respect to the inputs.

.PARAMETER WavDir
    Pre-generated mode: directory holding the WAV files named in the CSV. Defaults to
    the CSV's own directory. Not allowed without -Csv.

.PARAMETER Ffmpeg
    Path to ffmpeg(.exe). Defaults to the first ffmpeg on PATH, then to a bundled
    copy under dsp-core/build/reference-deps.

.PARAMETER WorkDir
    Native mode: where the WAVs and `ours.csv` go. Defaults to
    dsp-core/build/lufs-compare. Ignored in pre-generated mode.

.PARAMETER Tolerance
    Acceptance tolerance in LU. The standard's meter tolerance for integrated
    loudness is 0.1 LU; do not raise this to make a run pass.

.PARAMETER KeepWav
    Kept for compatibility with older command lines. WAV files are now always kept,
    so this switch changes nothing.

.PARAMETER CleanWav
    Native mode only: delete the WAV files this run generated, after the comparison.
    Never deletes pre-generated inputs (it is rejected in that mode).

.PARAMETER Report
    Markdown report path. Defaults to <input dir>/reference-report.md. The script
    refuses to replace an existing report unless -Force is also passed.

.PARAMETER Force
    Allow overwriting an existing report. Only use this deliberately; a previous
    report may be the only stored evidence for a comparison run.

.EXAMPLE
    pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1

.EXAMPLE
    pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1 `
        -Tool dsp-core/build/host/tools/Release/dg_lufs_compare.exe -Tolerance 0.1

.EXAMPLE
    pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1 `
        -Csv dsp-core/build/lufs-compare-android/ours.csv `
        -WavDir dsp-core/build/lufs-compare-android
#>
[CmdletBinding()]
param(
    [string]$Tool = '',
    [string]$Csv = '',
    [string]$WavDir = '',
    [string]$Ffmpeg = '',
    [string]$WorkDir = '',
    [double]$Tolerance = 0.1,
    [switch]$KeepWav,
    [switch]$CleanWav,
    [string]$Report = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if ($KeepWav) {
    Write-Host 'note: -KeepWav is now implied; WAV files are kept unless -CleanWav asks otherwise.'
}

function Fail([int]$code, [string]$message, [string[]]$hints = @()) {
    Write-Host ''
    Write-Host "BLOCKED: $message" -ForegroundColor Red
    foreach ($hint in $hints) { Write-Host "  $hint" }
    exit $code
}

# `-inf` is a legitimate reading meaning "no integrated value"; NaN/empty is not.
function Format-Lu($value) {
    if ($null -eq $value) { return 'n/a' }
    $text = [string]$value
    $text = $text.Trim()
    if ($text -eq '' -or $text -eq 'nan') { return 'n/a' }
    return $text
}

function Test-LuNumeric($value) {
    return ([string]$value).Trim() -match '^-?\d+(?:\.\d+)?$'
}

function Test-LuReferenceNone($value) {
    return ([string]$value).Trim() -match '^-?inf(inity)?$'
}

# Runs a native probe, captures stdout and stderr, and returns the real exit code.
# Do NOT put `Select-Object -First 1` on the native pipeline: stopping the pipeline
# early can tear down ffmpeg before it exits normally and leave a non-zero
# $LASTEXITCODE, which used to make a working ffmpeg look broken.
function Invoke-FfmpegProbe([string]$FilePath, [string[]]$Arguments) {
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $exitCode = $null
    $text = ''
    try {
        $captured = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
        # Wide formatting keeps one logical line per record, so the version regex
        # is not defeated by console-width wrapping.
        $text = ($captured | Out-String -Width 4096)
    }
    catch {
        $text = [string]$_.Exception.Message
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }
    $normalized = if ($null -eq $exitCode) { -1 } else { [int]$exitCode }
    return [pscustomobject]@{
        ExitCode = $normalized
        Text = $text
        Ok = ($null -ne $exitCode -and $exitCode -eq 0)
    }
}

# Reads every value of one metadata key from the text written by
# `ametadata=mode=print`. The key is a literal (`lavfi.r128.I`), not a regex.
function Get-MetadataValues([string]$text, [string]$key) {
    if ([string]::IsNullOrEmpty($text)) { return @() }
    $pattern = '(?m)^' + [regex]::Escape($key) + '=(-?inf(?:inity)?|-?\d+(?:\.\d+)?)[ \t\r]*$'
    $found = [regex]::Matches($text, $pattern)
    $values = New-Object System.Collections.Generic.List[string]
    foreach ($match in $found) { $values.Add($match.Groups[1].Value) }
    return $values.ToArray()
}

# True only when every 400 ms momentary block is strictly below the -70 LUFS
# absolute gate. That is the evidence that FFmpeg's `-70.000` integrated value is
# still the initialised sentinel and not a real measurement.
function Test-AllBlocksBelowAbsoluteGate([string[]]$momentaryValues) {
    $count = 0
    $minimum = $null
    $maximum = $null
    $allBelow = $true
    $allNumeric = $true
    foreach ($value in $momentaryValues) {
        $text = ([string]$value).Trim()
        if (Test-LuReferenceNone $text) { $count++; $allNumeric = $false; continue }
        if (-not (Test-LuNumeric $text)) {
            $allBelow = $false; $allNumeric = $false; $count++; continue
        }
        $number = [double]$text
        if ($null -eq $minimum -or $number -lt $minimum) { $minimum = $number }
        if ($null -eq $maximum -or $number -gt $maximum) { $maximum = $number }
        if ($number -ge -70.0) { $allBelow = $false }
        $count++
    }
    if ($count -eq 0) { $allBelow = $false; $allNumeric = $false }
    $range = if ($null -eq $minimum) { 'n/a' } else { ('{0:0.000}..{1:0.000}' -f $minimum, $maximum) }
    $witness = if ($allBelow) {
        "all $count M block(s) < -70.000 (range $range)"
    }
    elseif ($count -eq 0) {
        'no lavfi.r128.M blocks were found'
    }
    else {
        "$count M block(s), range $range; at least one is >= -70.000 or is not finite"
    }
    return [pscustomobject]@{
        AllBelow = $allBelow; AllNumeric = $allNumeric; Count = $count
        Minimum = $minimum; Maximum = $maximum; Range = $range; Witness = $witness
    }
}

# The integrated metadata value is exactly the initialised ABS_THRES sentinel.
# The metadata text has three decimals, so -70.000 is the only accepted spelling;
# numeric tolerance is only a guard against formatting changes.
function Test-MetadataSentinel($value) {
    if (-not (Test-LuNumeric $value)) { return $false }
    return [math]::Abs([double]$value + 70.0) -le 0.0005
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

# ------------------------------------------------------------------ pick the mode
$preGenerated = $false
if ($Csv -ne '') {
    $preGenerated = $true
}
elseif ($WavDir -ne '') {
    Fail 2 '-WavDir only makes sense together with -Csv (pre-generated mode).' @(
        'Native mode builds its own material; use -WorkDir to choose where it lands.',
        'Pre-generated mode:  -Csv <ours.csv> [-WavDir <dir>]'
    )
}
if ($preGenerated -and $CleanWav) {
    Fail 2 '-CleanWav is refused in pre-generated mode: the inputs are not ours to delete.' @(
        'Drop -CleanWav; the pulled WAVs and CSV stay exactly as they are.'
    )
}
if ($preGenerated -and $Tool -ne '') {
    Write-Host 'note: -Tool is ignored because -Csv selects pre-generated mode.'
}

if ($Tolerance -gt 0.1) {
    Write-Host ("note: tolerance {0} LU is wider than the standard's 0.1 LU; this run " -f $Tolerance) -ForegroundColor Yellow
    Write-Host '      is an exploration, not DG-012 acceptance evidence.' -ForegroundColor Yellow
}

$generatedWavs = @()

if (-not $preGenerated) {
    if ($WorkDir -eq '') { $WorkDir = Join-Path $root 'dsp-core\build\lufs-compare' }
    $WorkDir = [System.IO.Path]::GetFullPath($WorkDir)
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    $WavDir = $WorkDir
    $Csv = Join-Path $WorkDir 'ours.csv'
}
else {
    $Csv = [System.IO.Path]::GetFullPath($Csv)
    if (-not (Test-Path -LiteralPath $Csv -PathType Leaf)) {
        Fail 2 "CSV not found: $Csv" @(
            'Pull it from the device/emulator:  adb pull /data/local/tmp/lufs-compare <dir>',
            'The file must be the `ours.csv` written by `dg_lufs_compare --csv`.'
        )
    }
    if ($WavDir -eq '') { $WavDir = Split-Path -Parent $Csv }
    $WavDir = [System.IO.Path]::GetFullPath($WavDir)
    if (-not (Test-Path -LiteralPath $WavDir -PathType Container)) {
        Fail 2 "WAV directory not found: $WavDir" @('Pass -WavDir <dir> pointing at the pulled WAVs.')
    }
}
if ($Report -eq '') {
    $reportDir = if ($preGenerated) { Split-Path -Parent $Csv } else { $WorkDir }
    $Report = Join-Path $reportDir 'reference-report.md'
}
$Report = [System.IO.Path]::GetFullPath($Report)
if ((Test-Path -LiteralPath $Report) -and -not $Force) {
    Fail 2 "report already exists: $Report" @(
        'Pass a new -Report path to keep the existing evidence, or -Force to overwrite it deliberately.',
        'A previous comparison report is evidence; do not destroy it just to re-run.'
    )
}

# ---------------------------------------------------------------- find the tool
if (-not $preGenerated) {
    if ($Tool -eq '') {
        $candidates = @(
            (Join-Path $root 'dsp-core\build\host\tools\Release\dg_lufs_compare.exe'),
            (Join-Path $root 'dsp-core\build\host\tools\dg_lufs_compare.exe'),
            (Join-Path $root 'dsp-core\build\native\tools\Release\dg_lufs_compare.exe'),
            (Join-Path $root 'dsp-core\build\native\tools\dg_lufs_compare.exe'),
            (Join-Path $root 'dsp-core\build\host\dg_lufs_compare.exe'),
            (Join-Path $root 'dsp-core\build\native\dg_lufs_compare.exe')
        )
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath $candidate) { $Tool = $candidate; break }
        }
        if ($Tool -eq '') {
            $found = @(Get-ChildItem -Path (Join-Path $root 'dsp-core\build') -Filter 'dg_lufs_compare*' `
                    -Recurse -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Extension -eq '.exe' -or $_.Extension -eq '' })
            if ($found.Count -gt 0) { $Tool = $found[0].FullName }
        }
    }
    if ($Tool -eq '' -or -not (Test-Path -LiteralPath $Tool)) {
        Fail 2 'dg_lufs_compare binary not found.' @(
            'Build it first (host toolchain, not the Android NDK):',
            "  cmake -S `"$root\dsp-core`" -B `"$root\dsp-core\build\host`" -DDREAMGUARD_DSP_BUILD_TESTS=ON -DDREAMGUARD_DSP_BUILD_TOOLS=ON",
            "  cmake --build `"$root\dsp-core\build\host`" --config Release",
            'Or pass -Tool <path to dg_lufs_compare.exe>.',
            'Or use pre-generated mode: -Csv <ours.csv> -WavDir <dir> (no host compiler needed).'
        )
    }
}

# -------------------------------------------------------------- find the ffmpeg
if ($Ffmpeg -eq '') {
    $command = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -ne $command) { $Ffmpeg = $command.Source }
}
if ($Ffmpeg -eq '') {
    $bundled = @(Get-ChildItem -Path (Join-Path $root 'dsp-core\build\reference-deps') -Filter 'ffmpeg*.exe' `
            -Recurse -File -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)
    if ($bundled.Count -gt 0) { $Ffmpeg = $bundled[0].FullName }
}
if ($Ffmpeg -eq '' -or -not (Test-Path -LiteralPath $Ffmpeg)) {
    $selfcheckHint = if ($preGenerated) {
        'The readings in the CSV are untouched, but nothing was measured against a reference.'
    }
    else {
        "  & `"$Tool`" --selfcheck"
    }
    Fail 3 'ffmpeg not found; the reference side of DG-012 was NOT executed.' @(
        'Point at it with -Ffmpeg <path> (any build with the ebur128 filter).',
        'Verify with:  & <ffmpeg> -hide_banner -filters | Select-String ebur128',
        $selfcheckHint,
        'Do not mark DG-012 VERIFIED until this comparison has actually run.'
    )
}
$versionProbe = Invoke-FfmpegProbe $Ffmpeg @('-hide_banner', '-version')
# Deliberately no `^`/`$` anchors: Out-String joins native lines with CRLF on
# Windows, PowerShell may prefix stderr records with "ffmpeg.exe :", and neither
# detail should decide whether the binary ran. The exit code and the presence of
# this line are the two independent checks; the capture stops at CR/LF.
$versionMatch = [regex]::Match($versionProbe.Text, '(?i)(ffmpeg version[^\r\n]*)')
$ffmpegVersion = if ($versionMatch.Success) { $versionMatch.Groups[1].Value.Trim() } else { '' }
if (-not $versionProbe.Ok -or $ffmpegVersion -eq '') {
    $versionTail = (($versionProbe.Text -split "`r?`n") |
            Where-Object { $_.Trim() -ne '' } | Select-Object -Last 4) -join ' | '
    if ($versionTail.Length -gt 500) {
        $versionTail = $versionTail.Substring($versionTail.Length - 500)
    }
    Fail 3 "ffmpeg at $Ffmpeg failed the -version probe (exit $($versionProbe.ExitCode); no 'ffmpeg version' line): $versionTail" @(
        'Check the exact invocation by hand: & <ffmpeg> -hide_banner -version',
        'A non-zero exit or a missing version line is a real probe failure, not a reason to skip the reference.'
    )
}

# Both filters are required: ebur128 produces the values, ametadata prints the
# three-decimal metadata this script judges. A build missing either one cannot
# produce high-precision evidence, so stop instead of silently falling back to
# the one-decimal summary.
$filterProbe = Invoke-FfmpegProbe $Ffmpeg @('-hide_banner', '-filters')
$filterList = $filterProbe.Text
if (-not $filterProbe.Ok) {
    $filterTail = (($filterList -split "`r?`n") |
            Where-Object { $_.Trim() -ne '' } | Select-Object -Last 4) -join ' | '
    if ($filterTail.Length -gt 500) {
        $filterTail = $filterTail.Substring($filterTail.Length - 500)
    }
    Fail 3 "ffmpeg -filters failed (exit $($filterProbe.ExitCode)): $filterTail" @(
        'Check the exact invocation by hand: & <ffmpeg> -hide_banner -filters',
        'A non-zero exit is a real probe failure; do not skip the reference check.'
    )
}
if ($filterList -notmatch '(?m)\bebur128\b') {
    Fail 3 'this ffmpeg has no ebur128 filter; the reference side was NOT executed.' @(
        'Verify with:  & <ffmpeg> -hide_banner -filters | Select-String ebur128'
    )
}
if ($filterList -notmatch '(?m)\bametadata\b') {
    Fail 3 'this ffmpeg has no ametadata filter; the three-decimal metadata path cannot run.' @(
        'Verify with:  & <ffmpeg> -hide_banner -filters | Select-String ametadata',
        'Do not fall back to the summary I: line and call it a high-precision comparison.'
    )
}

# Metadata goes to a unique file in the OS temp directory. The filter takes a
# relative file name, so the child process is run from that directory; absolute
# WAV/report paths keep the rest of the script independent of the cwd.
$metaDir = [System.IO.Path]::GetTempPath()

Write-Host "mode   : $(if ($preGenerated) { 'pre-generated inputs' } else { 'native (tool generates)' })"
if (-not $preGenerated) { Write-Host "tool   : $Tool" }
Write-Host "csv    : $Csv"
Write-Host "wavs   : $WavDir"
Write-Host "ffmpeg : $Ffmpeg"
Write-Host "version: $ffmpegVersion"
Write-Host "tolerance: $Tolerance LU"
Write-Host ''

# ------------------------------------------------------------ build the material
if (-not $preGenerated) {
    Write-Host 'generating synthetic PCM and measuring it with dg_lufs ...'
    & $Tool --wav-dir $WorkDir --csv $Csv
    if ($LASTEXITCODE -ne 0) {
        Fail 2 "dg_lufs_compare failed with exit code $LASTEXITCODE." @(
            "Re-run it by hand to see the message:  & `"$Tool`" --wav-dir `"$WorkDir`" --csv `"$Csv`""
        )
    }
    if (-not (Test-Path -LiteralPath $Csv)) { Fail 2 "dg_lufs_compare did not write $Csv." }
}

$rows = @(Import-Csv -LiteralPath $Csv)
if ($rows.Count -eq 0) { Fail 2 "no cases in $Csv" }

# Sample completeness is checked before any measurement: duplicate case names or a
# missing column would make the report look complete while a case silently
# vanished or was judged from the wrong field.
$requiredColumns = @('case', 'wav', 'sample_rate', 'channels', 'integrated_lufs', 'expect_integrated')
$missingColumns = @($requiredColumns | Where-Object { $rows[0].PSObject.Properties.Name -notcontains $_ })
if ($missingColumns.Count -gt 0) {
    Fail 2 ("the readings CSV is missing column(s): " + ($missingColumns -join ', ')) @(
        "Expected columns: $($requiredColumns -join ', ')",
        'Use the CSV written by dg_lufs_compare, not a hand-trimmed copy.'
    )
}
$duplicateCases = @($rows | Group-Object -Property case | Where-Object { $_.Count -gt 1 })
if ($duplicateCases.Count -gt 0) {
    Fail 2 ('duplicate case name(s) in the CSV: ' + (($duplicateCases | ForEach-Object { $_.Name }) -join ', ')) @(
        'Every case must appear once; a duplicate would make the report ambiguous.'
    )
}

$csvHash = (Get-FileHash -LiteralPath $Csv -Algorithm SHA256).Hash

# ------------------------------------------------------------------- the compare
$results = New-Object System.Collections.Generic.List[object]
$missingWavs = 0

foreach ($row in $rows) {
    $wavName = [string]$row.wav
    $wav = if ([System.IO.Path]::IsPathRooted($wavName)) { $wavName } else { Join-Path $WavDir $wavName }
    if (-not $preGenerated) { $generatedWavs += $wav }
    # The tool writes bare file names; tolerate a CSV that kept a directory prefix
    # (a hand-edited or relocated CSV) by falling back to the leaf name.
    if (-not (Test-Path -LiteralPath $wav -PathType Leaf) -and $wavName -ne '') {
        $leaf = Split-Path -Leaf $wavName
        $alt = Join-Path $WavDir $leaf
        if (Test-Path -LiteralPath $alt -PathType Leaf) { $wav = $alt }
    }
    if (-not (Test-Path -LiteralPath $wav -PathType Leaf)) {
        $missingWavs++
        $results.Add([pscustomobject]@{
                case = $row.case; rate = [int]$row.sample_rate; channels = [int]$row.channels
                kind = $(if (([string]$row.expect_integrated).Trim() -match '^(1|true|yes)$') { 'numeric' } else { 'undefined' })
                ours = Format-Lu $row.integrated_lufs; reference = 'missing'
                summary = ''; delta = 'n/a'; status = 'FAIL'; noDataKind = ''; gateWitness = ''
                note = "WAV file missing: $wavName"; hash = ''
            })
        continue
    }
    $wavHash = (Get-FileHash -LiteralPath $wav -Algorithm SHA256).Hash

    # The metadata file name is relative because the filtergraph option separator
    # is ':' and a Windows absolute path would need fragile escaping. Run ffmpeg
    # with its cwd set to the temp directory, then read the unique file back.
    $metaName = 'dg012-meta-' + [guid]::NewGuid().ToString('N') + '.txt'
    $metaPath = Join-Path $metaDir $metaName
    $output = ''
    $ffmpegExit = 0
    $oldCurrentDirectory = [Environment]::CurrentDirectory
    try {
        [Environment]::CurrentDirectory = $metaDir
        Push-Location -LiteralPath $metaDir
        $output = (& $Ffmpeg -hide_banner -nostdin -nostats -i $wav `
                -af "ebur128=metadata=1:peak=true,ametadata=mode=print:file=$metaName" `
                -f null - 2>&1 | Out-String)
        $ffmpegExit = $LASTEXITCODE
    }
    finally {
        Pop-Location
        [Environment]::CurrentDirectory = $oldCurrentDirectory
    }

    $metaText = ''
    if (Test-Path -LiteralPath $metaPath -PathType Leaf) {
        $metaText = Get-Content -LiteralPath $metaPath -Raw -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $metaPath -Force -ErrorAction SilentlyContinue
    }
    if ($null -eq $metaText) { $metaText = '' }

    if ($ffmpegExit -ne 0) {
        $tail = ($output -split "`r?`n" | Where-Object { $_ -ne '' } | Select-Object -Last 6) -join ' | '
        if ($tail.Length -gt 400) { $tail = $tail.Substring($tail.Length - 400) }
        $results.Add([pscustomobject]@{
                case = $row.case; rate = [int]$row.sample_rate; channels = [int]$row.channels
                kind = 'numeric'; ours = Format-Lu $row.integrated_lufs; reference = 'error'
                summary = ''; delta = 'n/a'; status = 'FAIL'; noDataKind = ''; gateWitness = ''
                note = "ffmpeg exited $ffmpegExit : $tail"; hash = $wavHash
            })
        continue
    }

    $iValues = @(Get-MetadataValues $metaText 'lavfi.r128.I')
    $mValues = @(Get-MetadataValues $metaText 'lavfi.r128.M')
    if ($iValues.Count -eq 0) {
        $results.Add([pscustomobject]@{
                case = $row.case; rate = [int]$row.sample_rate; channels = [int]$row.channels
                kind = 'numeric'; ours = Format-Lu $row.integrated_lufs; reference = 'no-metadata'
                summary = ''; delta = 'n/a'; status = 'FAIL'; noDataKind = ''; gateWitness = ''
                note = 'ametadata=print produced no lavfi.r128.I value; the high-precision reference is unavailable'
                hash = $wavHash
            })
        continue
    }

    # The summary block prints "I:   -23.0 LUFS"; per-frame logs print the same
    # key earlier, so the last match is the final integrated value. It is only a
    # consistency witness now: the verdict uses the three-decimal metadata.
    $summaryPattern = 'I:\s*(-?inf(?:inity)?|-?\d+(?:\.\d+)?)\s*LUFS'
    $summaryMatches = [regex]::Matches($output, $summaryPattern)
    $summaryI = if ($summaryMatches.Count -gt 0) {
        $summaryMatches[$summaryMatches.Count - 1].Groups[1].Value
    }
    else { '' }

    $metadataI = $iValues[$iValues.Count - 1]
    $ours = ([string]$row.integrated_lufs).Trim()
    $expectIntegrated = ([string]$row.expect_integrated).Trim() -match '^(1|true|yes)$'
    $oursNone = -not (Test-LuNumeric $ours)
    $kind = if ($expectIntegrated) { 'numeric' } else { 'undefined' }
    $status = 'FAIL'
    $note = ''
    $delta = 'n/a'
    $noDataKind = ''
    $gateWitness = ''

    if ($expectIntegrated) {
        if ($oursNone) {
            $note = 'expected a numeric ours value, but the CSV reports none'
        }
        elseif (-not (Test-LuNumeric $metadataI)) {
            $note = "ffmpeg metadata I is '$metadataI', not a numeric value"
        }
        elseif (-not (Test-LuNumeric $summaryI)) {
            $note = 'no "I: ... LUFS" summary line; metadata could not be cross-checked'
        }
        else {
            $summaryDifference = [math]::Abs([double]$metadataI - [double]$summaryI)
            if ($summaryDifference -gt 0.0501) {
                $note = "metadata I=$metadataI and summary I=$summaryI disagree by more than half a summary step"
            }
            else {
                # -70.000 is also the initialised ABS_THRES sentinel. A numeric
                # "ours" value must not turn that sentinel into a PASS without
                # an independent M witness that at least one 400 ms block
                # actually reached the absolute gate.
                $referenceUsable = $true
                if (Test-MetadataSentinel $metadataI) {
                    $gate = Test-AllBlocksBelowAbsoluteGate $mValues
                    $gateWitness = $gate.Witness
                    if (-not $gate.AllNumeric) {
                        $referenceUsable = $false
                        $note = "reference metadata I=-70.000 but its M witness is missing or non-finite ($($gate.Witness)); the value cannot be used as a numeric reference"
                    }
                    elseif ($gate.AllBelow) {
                        $referenceUsable = $false
                        $note = "reference metadata I=-70.000 is the initialised sentinel ($($gate.Witness)); no 400 ms block reached the absolute gate, so this is not a numeric comparison"
                    }
                }

                if ($referenceUsable) {
                    $numericDelta = [double]$ours - [double]$metadataI
                    $delta = ('{0:0.000}' -f $numericDelta)
                    if ([math]::Abs($numericDelta) -le $Tolerance) {
                        $status = 'PASS'
                        $note = "metadata I (3 dp); summary I=$summaryI used only as a consistency check"
                    }
                    else {
                        $note = "|delta| exceeds $Tolerance LU; fix the analyser, do not widen the tolerance"
                    }
                }
            }
        }
    }
    else {
        # Digital silence, or a stream shorter than one 400 ms block. FFmpeg's
        # metadata always carries I, so -70.000 alone proves nothing. Require the
        # independent M witness: every 400 ms block below the absolute gate.
        $secondsText = if ($row.PSObject.Properties.Name -contains 'seconds') {
            ([string]$row.seconds).Trim()
        }
        else { '' }

        if (-not $oursNone) {
            $note = "definition broken on our side: this case has no integrated value, but dg_lufs reported $ours LUFS"
        }
        elseif (-not (Test-LuNumeric $summaryI)) {
            $note = 'no "I: ... LUFS" summary line; the -70.000 sentinel could not be cross-checked'
        }
        elseif ([math]::Abs([double]$metadataI - [double]$summaryI) -gt 0.0501) {
            $note = "metadata I=$metadataI and summary I=$summaryI disagree; the sentinel reading is unreliable"
        }
        elseif (-not (Test-MetadataSentinel $metadataI)) {
            $note = "reference metadata I=$metadataI is not the -70.000 sentinel; a numeric value for a no-data case must be reviewed"
        }
        else {
            $gate = Test-AllBlocksBelowAbsoluteGate $mValues
            $gateWitness = $gate.Witness
            if (-not $gate.AllBelow) {
                $note = "metadata I=-70.000 is ambiguous here: at least one 400 ms M block is >= -70.000 ($($gate.Range)); not accepted as no-data"
            }
            else {
                $status = 'NO-DATA'
                $noDataKind = if (([string]$row.signal).Trim() -eq 'silence') { 'silence' }
                              elseif ($secondsText -ne '' -and [double]$secondsText -lt 0.4) { 'sub-400ms' }
                              else { 'no-gating-block' }
                $note = "ffmpeg metadata I=$metadataI is the initialised ABS_THRES sentinel; $($gate.Witness); our side reports no value. Not a numeric comparison."
            }
        }
    }

    $results.Add([pscustomobject]@{
            case = $row.case; rate = [int]$row.sample_rate; channels = [int]$row.channels
            kind = $kind; ours = Format-Lu $ours; reference = Format-Lu $metadataI
            summary = Format-Lu $summaryI; delta = $delta; status = $status
            noDataKind = $noDataKind; gateWitness = $gateWitness; note = $note; hash = $wavHash
        })
}

# --------------------------------------------------------------------- the report
$passed = @($results | Where-Object { $_.status -eq 'PASS' }).Count
$failed = @($results | Where-Object { $_.status -eq 'FAIL' }).Count
$noData = @($results | Where-Object { $_.status -eq 'NO-DATA' }).Count
$noDataSilence = @($results | Where-Object { $_.noDataKind -eq 'silence' }).Count
$noDataShort = @($results | Where-Object { $_.noDataKind -eq 'sub-400ms' }).Count
$noDataNoGate = @($results | Where-Object { $_.noDataKind -eq 'no-gating-block' }).Count
$numericCases = @($results | Where-Object { $_.kind -eq 'numeric' }).Count
$deltas = @($results | Where-Object { $_.delta -match '^-?\d+(\.\d+)?$' } |
        ForEach-Object { [math]::Abs([double]$_.delta) })
$worst = if ($deltas.Count -gt 0) { ($deltas | Measure-Object -Maximum).Maximum } else { 0.0 }
$worstCase = if ($deltas.Count -gt 0) {
    $results | Where-Object { $_.delta -match '^-?\d+(\.\d+)?$' } |
        Sort-Object { [math]::Abs([double]$_.delta) } -Descending | Select-Object -First 1 -ExpandProperty case
}
else { '' }

function Format-MarkdownCell([string]$value) {
    if ($null -eq $value) { return '' }
    return (($value -replace '\|', '\|') -replace "`r?`n", ' ')
}

Write-Host ''
Write-Host ('{0,-32} {1,6} {2,3} {3,10} {4,10} {5,8}  {6}' -f 'case', 'rate', 'ch', 'ours', 'metadata', 'delta', 'status')
Write-Host ('-' * 96)
foreach ($r in $results) {
    $colour = switch ($r.status) {
        'PASS' { 'Green' }
        'FAIL' { 'Red' }
        'NO-DATA' { 'Yellow' }
        default { 'DarkGray' }
    }
    Write-Host ('{0,-32} {1,6} {2,3} {3,10} {4,10} {5,8}  {6}' -f `
            $r.case, $r.rate, $r.channels, $r.ours, $r.reference, $r.delta, $r.status) -ForegroundColor $colour
    if ($r.status -ne 'PASS' -and $r.note -ne '') {
        Write-Host ("    {0}" -f $r.note) -ForegroundColor $colour
    }
}

$noDataSummary = if ($noData -gt 0) {
    "$noData no-data case(s) reported separately and not counted as passing " +
    "(silence $noDataSilence, sub-400ms $noDataShort, no-gating-block $noDataNoGate)"
}
else { 'no no-data cases' }

$verdict = if ($failed -gt 0) {
    "FAIL: $failed case(s) did not pass (worst |delta| $('{0:0.000}' -f $worst) LU); " +
    'fix the root cause, do not widen the tolerance.'
}
elseif ($passed -eq 0) {
    "FAIL: no case produced a numeric comparison within $Tolerance LU; $noDataSummary."
}
else {
    "PASS: $passed numeric case(s) within $Tolerance LU (worst |delta| $('{0:0.000}' -f $worst) LU on $worstCase); " +
    "$noDataSummary."
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# DG-012 LUFS reference comparison — ffmpeg ebur128 metadata')
$lines.Add('')
$lines.Add("Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))")
$lines.Add('')
$lines.Add('| item | value |')
$lines.Add('| --- | --- |')
$lines.Add("| input mode | $(if ($preGenerated) { 'pre-generated CSV + WAV directory' } else { 'native: dg_lufs_compare generated the material' }) |")
if (-not $preGenerated) { $lines.Add("| our implementation | ``dg_lufs`` (dsp-core/src/lufs.cpp) via ``$Tool`` |") }
else { $lines.Add('| our implementation | `dg_lufs` (dsp-core/src/lufs.cpp), readings taken from the given CSV |') }
$lines.Add("| readings CSV | ``$Csv`` (SHA256 ``$csvHash``) |")
$lines.Add("| WAV directory | ``$WavDir`` |")
$lines.Add("| reference meter | ``$ffmpegVersion`` |")
$lines.Add("| ffmpeg path | ``$Ffmpeg`` |")
$lines.Add("| reference channel | ``lavfi.r128.I`` via ``ametadata=mode=print`` (3 decimal places; effective resolution about 0.01 LU) |")
$lines.Add("| tolerance | $Tolerance LU |")
$lines.Add('| cases in CSV | ' + ($results.Count) + ' |')
$lines.Add("| comparable numeric cases | $numericCases |")
$lines.Add("| passed / failed / no-data | $passed / $failed / $noData |")
$lines.Add("| no-data by cause | silence $noDataSilence, sub-400ms $noDataShort, no-gating-block $noDataNoGate |")
$lines.Add("| worst absolute deviation | $('{0:0.000}' -f $worst) LU" + $(if ($worstCase -ne '') { " (``$worstCase``)" } else { '' }) + ' |')
$lines.Add("| verdict | $verdict |")
$lines.Add('')
$lines.Add('## Command lines')
$lines.Add('')
$lines.Add('```text')
if (-not $preGenerated) { $lines.Add("$Tool --wav-dir `"$WorkDir`" --csv `"$Csv`"") }
$lines.Add("$Ffmpeg -hide_banner -nostdin -nostats -i <case>.wav ``")
$lines.Add("  -af `"ebur128=metadata=1:peak=true,ametadata=mode=print:file=<temp>.txt`" -f null -")
$lines.Add('```')
$lines.Add('')
$lines.Add('`ametadata` writes to a unique temporary file because a Windows absolute path')
$lines.Add('inside a filtergraph would need fragile escaping. Only the last `lavfi.r128.I`')
$lines.Add('value is the final integrated value; earlier copies appear per 100 ms frame.')
$lines.Add('')
$lines.Add('## Results')
$lines.Add('')
$lines.Add('| case | rate | ch | kind | ours (LUFS) | ffmpeg metadata I | ffmpeg summary I | delta (LU) | status | no-data kind | note |')
$lines.Add('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
foreach ($r in $results) {
    $lines.Add("| $($r.case) | $($r.rate) | $($r.channels) | $($r.kind) | $($r.ours) | $($r.reference) | $($r.summary) | $($r.delta) | $($r.status) | $($r.noDataKind) | $(Format-MarkdownCell $r.note) |")
}
$lines.Add('')
$lines.Add('### No-data cases: silence and streams shorter than one 400 ms block')
$lines.Add('')
$lines.Add('FFmpeg initialises integrated loudness to `ABS_THRES = -70` and its metadata')
$lines.Add('`SET_META` for `lavfi.r128.I` runs unconditionally. A silent stream and a real')
$lines.Add('-70 LUFS stream therefore both print `-70.000`. Treating that number alone as')
$lines.Add('a pass would be wrong. The script requires all of the following before it')
$lines.Add('records `NO-DATA`:')
$lines.Add('')
$lines.Add('1. Our side reports no integrated value for the case.')
$lines.Add('2. The metadata and summary readings agree within half a summary step.')
$lines.Add('3. Metadata `I` is the `-70.000` sentinel.')
$lines.Add('4. Every 400 ms `lavfi.r128.M` block is strictly below -70.000 LUFS.')
$lines.Add('')
$lines.Add('Condition 4 is the independent witness: at least one momentary block has to')
$lines.Add('clear the absolute gate before any block can enter the integrated histogram,')
$lines.Add('so the sentinel is not ambiguous in that run. If any `M` is at or above -70,')
$lines.Add('the case is `FAIL`, not `NO-DATA`.')
$lines.Add('')
$lines.Add('| case | cause | witness | status |')
$lines.Add('| --- | --- | --- | --- |')
$noDataRows = @($results | Where-Object { $_.status -eq 'NO-DATA' })
if ($noDataRows.Count -eq 0) {
    $lines.Add('| — | none | — | — |')
}
else {
    foreach ($r in $noDataRows) {
        $lines.Add("| $($r.case) | $($r.noDataKind) | $(Format-MarkdownCell $r.gateWitness) | $($r.status) |")
    }
}
$lines.Add('')
$lines.Add('`NO-DATA` is a definitional outcome, not numeric agreement and not a pass. It')
$lines.Add('is reported separately so silence and sub-block material cannot inflate the')
$lines.Add('pass count. The cause is taken from the CSV: `signal=silence` means digital')
$lines.Add('silence; `seconds < 0.4` means a stream too short for one gating block; the')
$lines.Add('remaining cause is a stream with no block above the absolute gate.')
$lines.Add('')
$lines.Add('### Sign conventions and scope')
$lines.Add('')
$lines.Add('- `ours` is `dg_lufs_values.integrated_lufs` after the whole stream was processed.')
$lines.Add('- `ffmpeg metadata I` is the last `lavfi.r128.I` line, printed with `%.3f`.')
$lines.Add('- `ffmpeg summary I` is the last `I: ... LUFS` line. It has only 0.1 LU')
$lines.Add('  resolution and is used only as a consistency check: a difference above')
$lines.Add('  0.0501 LU between metadata and summary is a `FAIL`, so a mis-parse cannot')
$lines.Add('  silently produce a verdict.')
$lines.Add('- `delta = ours - ffmpeg metadata I` in LU; the acceptance criterion is')
$lines.Add('  `|delta| <= tolerance`. The tolerance is not widened to make a case pass.')
$lines.Add('- The metadata prints three decimals, but FFmpeg accumulates the integrated')
$lines.Add('  value in 0.01 LU histogram bins (`HIST_GRAIN=100`). The third decimal is')
$lines.Add('  therefore formatting resolution, not measurement resolution.')
$lines.Add('- WAV SHA256 per case (proves the reference measured the same bytes we did):')
$lines.Add('')
$lines.Add('| case | wav sha256 |')
$lines.Add('| --- | --- |')
foreach ($r in $results) {
    $lines.Add("| $($r.case) | $($r.hash) |")
}
$lines.Add('')
$lines.Add('- A non-zero exit code from this script means the comparison did NOT pass.')
$lines.Add('- `NO-DATA` cases never make the exit code zero on their own: at least one')
$lines.Add('  numeric case must pass, and no case may fail.')
$lines.Add('- The input CSV and WAV files are never deleted by this script.')
$lines.Add('')
$lines.Add('This is a cross-check against a specific FFmpeg build, not a standards-body')
$lines.Add('certification of either meter. A report is evidence only when produced by an')
$lines.Add('actual run. A report that was never generated leaves DG-012 at')
$lines.Add('IN_PROGRESS / PROVISIONAL.')
$lines.Add('')
Set-Content -LiteralPath $Report -Value ($lines -join [Environment]::NewLine) -Encoding UTF8

# A machine-readable sidecar next to the markdown report.
$sidecar = [System.IO.Path]::ChangeExtension($Report, '.csv')
if ($sidecar -eq $Csv) { $sidecar = [System.IO.Path]::ChangeExtension($Report, '.result.csv') }
$results | Export-Csv -LiteralPath $sidecar -NoTypeInformation -Encoding UTF8

# -------------------------------------------------------------------- the cleanup
$removed = 0
if (-not $preGenerated -and $CleanWav) {
    foreach ($wav in $generatedWavs) {
        if (Test-Path -LiteralPath $wav) {
            Remove-Item -LiteralPath $wav -Force -ErrorAction SilentlyContinue
            $removed++
        }
    }
}

Write-Host ''
Write-Host $verdict
if ($missingWavs -gt 0) { Write-Host "missing WAV files: $missingWavs" -ForegroundColor Red }
Write-Host "report: $Report"
Write-Host "sidecar: $sidecar"
Write-Host "readings: $Csv"
if ($removed -gt 0) { Write-Host "removed $removed generated WAV file(s) (-CleanWav)." }
else { Write-Host 'input files kept (nothing was deleted).' }

if ($failed -gt 0 -or $passed -eq 0) { exit 1 }
exit 0
