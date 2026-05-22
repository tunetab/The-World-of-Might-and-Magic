$ErrorActionPreference = 'Stop'

function Get-WmmaToolLockPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $normalizedRoot = (Resolve-Path -LiteralPath $Root).Path.ToLowerInvariant()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($normalizedRoot)
        $hashBytes = $sha.ComputeHash($bytes)
        $hash = -join ($hashBytes | ForEach-Object { $_.ToString('x2') })
    } finally {
        $sha.Dispose()
    }

    return Join-Path ([System.IO.Path]::GetTempPath()) "wmma_tool_$hash.lock"
}

function Enter-WmmaToolLock {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [string]$Name = 'tool',

        [int]$TimeoutSeconds = 90
    )

    if (-not $global:WmmaToolLocks) {
        $global:WmmaToolLocks = @{}
    }

    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    $rootKey = $resolvedRoot.ToLowerInvariant()

    if ($global:WmmaToolLocks.ContainsKey($rootKey)) {
        $entry = $global:WmmaToolLocks[$rootKey]
        $entry.Depth++
        return [pscustomobject]@{
            RootKey = $rootKey
            Reentrant = $true
        }
    }

    $lockPath = Get-WmmaToolLockPath -Root $resolvedRoot
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $stream = $null

    while (-not $stream) {
        try {
            $stream = [System.IO.File]::Open(
                $lockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None
            )
        } catch [System.IO.IOException] {
            if ((Get-Date) -ge $deadline) {
                throw "Timed out waiting for tool lock: $lockPath"
            }

            Start-Sleep -Milliseconds 200
        }
    }

    $entry = [pscustomobject]@{
        Stream = $stream
        Depth = 1
        Path = $lockPath
        Root = $resolvedRoot
    }
    $global:WmmaToolLocks[$rootKey] = $entry

    $lockInfo = @(
        "pid=$PID",
        "tool=$Name",
        "root=$resolvedRoot",
        "started_utc=$([DateTime]::UtcNow.ToString('o'))"
    ) -join "`n"

    $lockBytes = [System.Text.Encoding]::UTF8.GetBytes($lockInfo)
    $stream.SetLength(0)
    $stream.Write($lockBytes, 0, $lockBytes.Length)
    $stream.Flush()

    return [pscustomobject]@{
        RootKey = $rootKey
        Reentrant = $false
    }
}

function Exit-WmmaToolLock {
    param(
        [object]$Lock
    )

    if (-not $Lock -or -not $global:WmmaToolLocks) {
        return
    }

    $rootKey = $Lock.RootKey
    if (-not $rootKey -or -not $global:WmmaToolLocks.ContainsKey($rootKey)) {
        return
    }

    $entry = $global:WmmaToolLocks[$rootKey]
    $entry.Depth--

    if ($entry.Depth -gt 0) {
        return
    }

    try {
        $entry.Stream.Dispose()
    } finally {
        $global:WmmaToolLocks.Remove($rootKey)
    }
}

function Invoke-WmmaToolMain {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [scriptblock]$ScriptBlock,

        [Alias('Name')]
        [string]$ToolName = 'tool'
    )

    $lock = Enter-WmmaToolLock -Root $Root -Name $ToolName
    try {
        & $ScriptBlock
    } finally {
        Exit-WmmaToolLock -Lock $lock
    }
}
