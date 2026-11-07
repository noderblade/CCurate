# Run from the extracted package. No installed SDK or development tools are needed.
param(
    [switch]$NoPause,
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$AppArguments
)
$ErrorActionPreference = 'Continue'
$OutputEncoding = [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$directory = $env:CCURATE_LOG_DIR
if (-not $directory) {
    $directory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CCurate\Logs'
}
try {
    $directory = [IO.Path]::GetFullPath($directory)
    New-Item -ItemType Directory -Path $directory -Force -ErrorAction Stop | Out-Null
} catch {
    $directory = Join-Path ([IO.Path]::GetTempPath()) 'CCurate\Logs'
    New-Item -ItemType Directory -Path $directory -Force -ErrorAction Stop | Out-Null
}
$env:CCURATE_LOG_DIR = $directory
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + $PID
$transcript = Join-Path $directory ('launcher-' + $stamp + '.log')
$nativeOutput = Join-Path $directory ('console-' + $stamp + '.log')
$started = [DateTime]::UtcNow
Start-Transcript -LiteralPath $transcript -ErrorAction Stop | Out-Null
$code = 2
try {
    Write-Host 'CCurate Windows startup diagnostics'
    Write-Host ('UTC: ' + $started.ToString('o'))
    Write-Host ('Windows: ' + [Environment]::OSVersion.VersionString)
    Write-Host ('64-bit operating system: ' + [Environment]::Is64BitOperatingSystem)
    Write-Host ('PowerShell: ' + $PSVersionTable.PSVersion.ToString())
    Write-Host ('Logs: ' + $directory)
    $exe = Join-Path $PSScriptRoot 'ccurate.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw 'ccurate.exe is missing. Extract the complete ZIP before running this launcher.'
    }
    # Use .NET directly: an inherited PowerShell 7 PSModulePath can prevent
    # Windows PowerShell from finding its Get-FileHash script module.
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            $stream = [IO.File]::OpenRead($exe)
            try { $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
        } finally { $algorithm.Dispose() }
        Write-Host ('Executable SHA-256: ' + $hash)
    } catch {
        Write-Host ('Executable checksum unavailable: ' + $_.Exception.Message)
    }
    # A child inherits the parent's error mode. Suppress loader error dialogs so
    # a missing DLL produces an exit code instead of a hidden, blocking popup.
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class CCurateDebugNative {
    [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
}
'@ -ErrorAction Stop
    $oldMode = [CCurateDebugNative]::SetErrorMode(0x8003)
    try {
        $global:LASTEXITCODE = $null
        & $exe --debug @AppArguments 2>&1 | Tee-Object -FilePath $nativeOutput
        if ($null -ne $LASTEXITCODE) { $code = [int]$LASTEXITCODE }
    } finally {
        [CCurateDebugNative]::SetErrorMode($oldMode) | Out-Null
    }
    $unsigned = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$code), 0)
    $hex = '0x{0:X8}' -f $unsigned
    Write-Host ("Process exit code: {0} ({1})" -f $code, $hex)
    $reasons = @{
        '0xC0000135' = 'A required DLL could not be loaded. Check that the complete ZIP was extracted.'
        '0xC000007B' = 'Invalid executable/DLL image or mixed 32-bit and 64-bit binaries.'
        '0xC0000142' = 'A DLL failed during initialization.'
        '0xC000001D' = 'Illegal instruction. Check the CPU features exposed by the virtual machine.'
        '0xC0000005' = 'Access violation. Include the startup log and this launcher log in the report.'
        '0xC0000409' = 'Windows terminated the process after a fail-fast error.'
    }
    if ($reasons.ContainsKey($hex)) { Write-Host $reasons[$hex] }
    $logs = @(Get-ChildItem -LiteralPath $directory -Filter 'startup-*.log' |
        Where-Object { $_.CreationTimeUtc -ge $started } | Sort-Object LastWriteTimeUtc)
    if ($logs.Count) {
        foreach ($log in $logs) { Write-Host ('Application startup log: ' + $log.FullName) }
    } else {
        Write-Host 'No application startup log was created. Windows may have stopped before the entry point.'
    }
} catch {
    Write-Host ('Launcher error: ' + $_.Exception.Message)
    $code = 2
} finally {
    Write-Host ('Launcher log: ' + $transcript)
    Write-Host ('Console capture: ' + $nativeOutput)
    Stop-Transcript | Out-Null
    if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
}
exit $code
