# Copyright (C) 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile the actual smoke observer and exercise its formatter without invoking
# any native Windows API. Run with PowerShell 7 on Linux or Windows.
$ErrorActionPreference = "Stop"
$smoke = Join-Path $PSScriptRoot "../ci/smoke_windows_review_bundle.ps1"
$tokens = $null
$errors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile(
    $smoke, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw "Smoke script has PowerShell parse errors" }
$source = Get-Content -Raw -LiteralPath $smoke
$blocks = @([regex]::Matches($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@") |
    Where-Object { $_.Groups[1].Value.Contains("class AtrinikReviewCloseTimeoutProbe") })
if ($blocks.Count -ne 1) { throw "Expected one actual timeout observer" }
Add-Type -TypeDefinition $blocks[0].Groups[1].Value
$formatter = [AtrinikReviewCloseTimeoutProbe].GetMethod("FormatWaitChain",
    [System.Reflection.BindingFlags]"NonPublic,Static")
if ($null -eq $formatter) { throw "Expected wait-chain formatter" }
$count = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(4)
$cycle = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(4)
$nodes = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(280 * 16)
try {
    [System.Runtime.InteropServices.Marshal]::Copy([byte[]]::new(280 * 16), 0, $nodes, 280 * 16)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($count, 3)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($cycle, 1)
    # Main thread -> named critical section -> another owned thread.
    foreach ($entry in @(
        @(0, 8), @(4, 3), @(8, 101), @(12, 202),
        @(280, 1), @(284, 6),
        @(560, 8), @(564, 2), @(568, 101), @(572, 303)
    )) {
        [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, $entry[0], $entry[1])
    }
    $privateName = [System.Text.Encoding]::Unicode.GetBytes("PRIVATE-OBJECT-NAME")
    [System.Runtime.InteropServices.Marshal]::Copy($privateName, 0,
        [IntPtr]::Add($nodes, 288), $privateName.Length)
    function Format-Chain([uint32]$Status = 0) {
        return $formatter.Invoke($null, @([uint32]101, $Status, $count, $nodes, $cycle))
    }
    $expected = "wct_nodes=3 cycle=True; type=Thread status=Blocked thread=202; " +
        "type=CriticalSection status=Owned; type=Thread status=Running thread=303"
    if ((Format-Chain) -cne $expected) { throw "Owned wait-chain fixture mismatch" }

    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 568, 404)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 572, 505)
    $expected = "wct_nodes=3 cycle=True; type=Thread status=Blocked thread=202; " +
        "type=CriticalSection status=Owned; external_thread_boundary"
    if ((Format-Chain) -cne $expected) { throw "External identity boundary mismatch" }

    [System.Runtime.InteropServices.Marshal]::WriteInt32($count, 1)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($cycle, 0)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 0, 2147483647)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 4, -1)
    if ((Format-Chain) -cne "wct_nodes=1 cycle=False; type=Unknown status=Unknown") {
        throw "Unknown enum fixture mismatch"
    }
    foreach ($invalidCount in @(0, 17, 2147483647, -1)) {
        [System.Runtime.InteropServices.Marshal]::WriteInt32($count, $invalidCount)
        if ((Format-Chain) -cne "wct_invalid_count") { throw "Node bound fixture mismatch" }
    }
    # Error callbacks need not provide valid result pointers. Never dereference them.
    foreach ($status in @(5, 50, 234, 1223)) {
        $observed = $formatter.Invoke($null, @([uint32]101, [uint32]$status,
            [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero))
        if ($observed -cne "wct_error=$status") { throw "Error callback fixture mismatch" }
    }
    Write-Host "PowerShell parse, actual C# compilation and wait-chain formatter fixtures passed."
    Write-Host "Native WCT APIs were not invoked; Windows execution remains required."
} finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($nodes)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($cycle)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($count)
}
