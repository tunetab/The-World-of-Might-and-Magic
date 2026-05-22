param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^FRONT-[A-Z0-9-]+$')]
    [string]$Id,

    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Description = '',

    [ValidateSet('критический', 'высокий', 'средний', 'низкий')]
    [string]$Priority = 'средний',

    [string]$Participants = 'Уточнить.',

    [string]$State = 'Уточнить.',

    [string]$Risk = 'Уточнить.',

    [string]$Trigger = 'Уточнить.',

    [string[]]$Links = @(),

    [switch]$Urgent,

    [string]$UrgentSummary = '',

    [string]$Timer = '',

    [string]$TimerStatus = 'активен',

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$registryPath = Join-Path $root '09_Реестры\Фронты.json'

function Normalize-LinkReference {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    return ($Value.Trim().Trim('`') -replace '\\', '/')
}

if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "Front registry is missing: 09_Реестры/Фронты.json. Run .\tools\Собрать_фронты.ps1 -ImportFromMarkdown once."
}

if ([string]::IsNullOrWhiteSpace($Description)) {
    $Description = $Name
}

$registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath) | ConvertFrom-Json
$fronts = @($registry.fronts)
if ($fronts | Where-Object { $_.id -eq $Id } | Select-Object -First 1) {
    throw "FRONT-ID already exists: $Id"
}

$fronts += [pscustomobject][ordered]@{
    id = $Id
    name = $Description
}
$registry.fronts = @($fronts)

$activeFronts = @($registry.active_fronts)
$activeFronts += [pscustomobject][ordered]@{
    id = $Id
    front = $Name
    participants = $Participants
    state = $State
    risk = $Risk
    next_trigger = $Trigger
}
$registry.active_fronts = @($activeFronts)

if ($Urgent) {
    $linkReferences = @(
        $Links |
            ForEach-Object { Normalize-LinkReference -Value $_ } |
            Where-Object { $_ }
    )
    $urgentText = if ([string]::IsNullOrWhiteSpace($UrgentSummary)) { $State } else { $UrgentSummary }
    $urgentForks = @($registry.urgent_forks)
    $urgentForks += [pscustomobject][ordered]@{
        id = $Id
        priority = $Priority
        front = $Name
        summary = $urgentText
        trigger = $Trigger
        links = @($linkReferences)
    }
    $registry.urgent_forks = @($urgentForks)
}

if (-not [string]::IsNullOrWhiteSpace($Timer)) {
    $timers = @($registry.timers)
    $timers += [pscustomobject][ordered]@{
        id = $Id
        timer = $Timer
        status = $TimerStatus
        trigger = $Trigger
    }
    $registry.timers = @($timers)
}

$registry.updated_real_date = Get-Date -Format 'yyyy-MM-dd'
$json = ($registry | ConvertTo-Json -Depth 8).TrimEnd() + "`n"
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($registryPath, $json, $encoding)

& (Join-Path $root 'tools\Собрать_фронты.ps1') -SkipCheck
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $root 'tools\Собрать_панель_хода.ps1') -SkipCheck
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Created front: $Id"
}
