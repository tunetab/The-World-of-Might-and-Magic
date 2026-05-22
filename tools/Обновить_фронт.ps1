param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^FRONT-[A-Z0-9-]+$')]
    [string]$FrontId,

    [ValidateSet('критический', 'высокий', 'средний', 'низкий')]
    [string]$Priority,

    [string]$UrgentSummary,

    [string]$UrgentTrigger,

    [string]$State,

    [string]$Risk,

    [string]$NextTrigger,

    [string]$TimerStatus,

    [string]$TimerTrigger,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$registryPath = Join-Path $root '09_Реестры\Фронты.json'

function Normalize-Cell {
    param([string]$Value)

    return (($Value.Trim() -replace '\r?\n', ' ') -replace '\|', '/')
}

if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "Front registry is missing: 09_Реестры/Фронты.json. Run .\tools\Собрать_фронты.ps1 -ImportFromMarkdown once."
}

$registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath) | ConvertFrom-Json
$fronts = @($registry.fronts)
if (-not ($fronts | Where-Object { $_.id -eq $FrontId } | Select-Object -First 1)) {
    throw "FRONT-ID is not found: $FrontId"
}

$changed = $false

if ($Priority -or $UrgentSummary -or $UrgentTrigger) {
    $urgentForks = @($registry.urgent_forks)
    $urgentFork = $urgentForks | Where-Object { $_.id -eq $FrontId } | Select-Object -First 1
    if ($null -eq $urgentFork) {
        throw "Cannot find urgent fork row for $FrontId."
    }

    if ($Priority) {
        $urgentFork.priority = Normalize-Cell $Priority
    }

    if ($UrgentSummary) {
        $urgentFork.summary = Normalize-Cell $UrgentSummary
    }

    if ($UrgentTrigger) {
        $urgentFork.trigger = Normalize-Cell $UrgentTrigger
    }

    $registry.urgent_forks = @($urgentForks)
    $changed = $true
}

if ($State -or $Risk -or $NextTrigger) {
    $activeFronts = @($registry.active_fronts)
    $activeFront = $activeFronts | Where-Object { $_.id -eq $FrontId } | Select-Object -First 1
    if ($null -eq $activeFront) {
        throw "Cannot find active front row for $FrontId."
    }

    if ($State) {
        $activeFront.state = Normalize-Cell $State
    }

    if ($Risk) {
        $activeFront.risk = Normalize-Cell $Risk
    }

    if ($NextTrigger) {
        $activeFront.next_trigger = Normalize-Cell $NextTrigger
    }

    $registry.active_fronts = @($activeFronts)
    $changed = $true
}

if ($TimerStatus -or $TimerTrigger) {
    $timers = @($registry.timers)
    $timer = $timers | Where-Object { $_.id -eq $FrontId } | Select-Object -First 1
    if ($null -eq $timer) {
        throw "Cannot find timer row for $FrontId."
    }

    if ($TimerStatus) {
        $timer.status = Normalize-Cell $TimerStatus
    }

    if ($TimerTrigger) {
        $timer.trigger = Normalize-Cell $TimerTrigger
    }

    $registry.timers = @($timers)
    $changed = $true
}

if (-not $changed) {
    throw 'Nothing to update. Pass at least one update parameter.'
}

$registry.updated_real_date = Get-Date -Format 'yyyy-MM-dd'
$json = ($registry | ConvertTo-Json -Depth 8).TrimEnd() + "`n"
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($registryPath, $json, $encoding)

& (Join-Path $root 'tools\Собрать_фронты.ps1') -SkipCheck
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Собрать_панель_хода.ps1') -SkipCheck
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Updated front: $FrontId"
}
