param(
    [ValidatePattern('^DEC-PENDING-\d{3}$')]
    [string]$Id,

    [Parameter(Mandatory = $true)]
    [string]$Question,

    [string]$Owner,

    [ValidateSet('критический', 'высокий', 'средний', 'низкий')]
    [string]$Priority = 'высокий',

    [ValidateSet('active', 'waiting', 'later')]
    [string]$PanelStatus = 'active',

    [Parameter(Mandatory = $true)]
    [string]$PlayerCharacter,

    [string]$Scene = 'Уточнить сцену.',

    [string]$StoryDate = 'уточнить',

    [string]$Choice,

    [string]$PlayerAddition = 'ожидает решения.',

    [string]$ImmediateEffect = 'ожидает решения.',

    [string]$LongTermConsequences = 'ожидает решения.',

    [string[]]$Links = @('01_Кампания/03_Нерешенные_вопросы.md'),

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$registryPath = Join-Path $root '09_Реестры\Решения.json'

if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "Decision registry is missing: 09_Реестры/Решения.json. Run .\tools\Собрать_решения.ps1 -ImportFromMarkdown once."
}

function Read-Text {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }

    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
}

function Test-ProjectPath {
    param([string]$Reference)

    if ([string]::IsNullOrWhiteSpace($Reference)) {
        return $false
    }

    $cleanReference = $Reference.Trim().Trim('`')
    if ($cleanReference -match '^[a-z][a-z0-9+.-]*://') {
        return $true
    }

    $normalized = $cleanReference -replace '/', '\'
    if ([System.IO.Path]::IsPathRooted($normalized)) {
        return Test-Path -LiteralPath $normalized
    }

    return Test-Path -LiteralPath (Join-Path $root $normalized)
}

function Format-LinkText {
    param([string[]]$RawLinks)

    $tick = [char]96
    $formatted = New-Object 'System.Collections.Generic.List[string]'

    foreach ($link in $RawLinks) {
        if ([string]::IsNullOrWhiteSpace($link)) {
            continue
        }

        $clean = $link.Trim().Trim('`')
        if (-not (Test-ProjectPath -Reference $clean)) {
            throw "Decision link points to missing file: $clean"
        }

        $formatted.Add("$tick$clean$tick") | Out-Null
    }

    if ($formatted.Count -eq 0) {
        throw 'At least one valid decision link is required.'
    }

    return ($formatted.ToArray() -join ', ')
}

function Get-NextPendingDecisionId {
    param([object]$Registry)

    $decisionLogPath = Join-Path $root '01_Кампания\02_Журнал_решений.md'
    $openQuestionsPath = Join-Path $root '01_Кампания\03_Нерешенные_вопросы.md'
    $currentContextPath = Join-Path $root '01_Кампания\00_Текущий_контекст.md'
    $haystack = @(
        $Registry | ConvertTo-Json -Depth 8
        Read-Text -Path $decisionLogPath
        Read-Text -Path $openQuestionsPath
        Read-Text -Path $currentContextPath
    ) -join "`n"

    $numbers = @(
        [regex]::Matches($haystack, '\bDEC(?:-PENDING)?-(\d{3})\b') |
            ForEach-Object { [int]$_.Groups[1].Value }
    )

    $max = 0
    if ($numbers.Count -gt 0) {
        $max = [int](($numbers | Measure-Object -Maximum).Maximum)
    }

    return ('DEC-PENDING-{0:D3}' -f ($max + 1))
}

function Update-CurrentContext {
    param(
        [string]$PendingId,
        [string]$PendingQuestion,
        [string]$PendingOwner,
        [string]$PendingStatus
    )

    $contextPath = Join-Path $root '01_Кампания\00_Текущий_контекст.md'
    if (-not (Test-Path -LiteralPath $contextPath)) {
        return
    }

    $lines = @(Get-Content -Encoding UTF8 -LiteralPath $contextPath)
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -eq '## Немедленные решения') {
            $start = $i
            break
        }
    }

    if ($start -lt 0) {
        return
    }

    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^##\s+') {
            $end = $i
            break
        }
    }

    $before = @()
    if ($start -gt 0) {
        $before = @($lines[0..$start])
    } else {
        $before = @($lines[$start])
    }

    $section = @()
    if ($end -gt ($start + 1)) {
        $section = @($lines[($start + 1)..($end - 1)])
    }

    $after = @()
    if ($end -lt $lines.Count) {
        $after = @($lines[$end..($lines.Count - 1)])
    }

    $filtered = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in $section) {
        if ($line -match '^Активных `DEC-PENDING-\*` сейчас нет') {
            continue
        }

        if ($line -match [regex]::Escape($PendingId)) {
            continue
        }

        $filtered.Add($line) | Out-Null
    }

    $insertAt = $filtered.Count
    for ($i = 0; $i -lt $filtered.Count; $i++) {
        if ($filtered[$i] -eq 'Главные следующие узлы:') {
            $insertAt = $i
            break
        }
    }

    $pendingLine = '1. `{0}` - {1} (владелец: {2}; статус: {3}).' -f $PendingId, $PendingQuestion, $PendingOwner, $PendingStatus
    $filtered.Insert($insertAt, $pendingLine)

    if ($insertAt -gt 0 -and -not [string]::IsNullOrWhiteSpace($filtered[$insertAt - 1])) {
        $filtered.Insert($insertAt, '')
        $insertAt++
    }

    if ($insertAt + 1 -lt $filtered.Count -and -not [string]::IsNullOrWhiteSpace($filtered[$insertAt + 1])) {
        $filtered.Insert($insertAt + 1, '')
    }

    $renumbered = New-Object 'System.Collections.Generic.List[string]'
    $counter = 1
    foreach ($line in $filtered) {
        if ($line -match '^\d+\.\s+`DEC-PENDING-\d{3}`') {
            $renumbered.Add(($line -replace '^\d+\.', "$counter.")) | Out-Null
            $counter++
        } else {
            $renumbered.Add($line) | Out-Null
        }
    }

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($contextPath, ((@($before) + @($renumbered.ToArray()) + @($after)) -join "`n").TrimEnd() + "`n", $encoding)
}

if ([string]::IsNullOrWhiteSpace($Owner)) {
    $Owner = $PlayerCharacter
}

if ([string]::IsNullOrWhiteSpace($Choice)) {
    $Choice = "ожидает решения: $Question"
}

$registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath) | ConvertFrom-Json
$decisions = @($registry.decisions)

if ([string]::IsNullOrWhiteSpace($Id)) {
    $Id = Get-NextPendingDecisionId -Registry $registry
}

if (@($decisions | Where-Object { $_.id -eq $Id }).Count -gt 0) {
    throw "Decision ID already exists in 09_Реестры/Решения.json: $Id"
}

if ($Id -match '^DEC-PENDING-(\d{3})$') {
    $acceptedId = 'DEC-{0}' -f $Matches[1]
    if (@($decisions | Where-Object { $_.id -eq $acceptedId }).Count -gt 0) {
        throw "Accepted decision with the same number already exists: $acceptedId"
    }
}

$linksText = Format-LinkText -RawLinks $Links

$decisions += [pscustomobject][ordered]@{
    id = $Id
    state = 'pending'
    real_date = 'ожидает решения'
    story_date = $StoryDate
    player_character = $PlayerCharacter
    scene = $Scene
    choice = $Choice
    player_addition = $PlayerAddition
    immediate_effect = $ImmediateEffect
    long_term_consequences = $LongTermConsequences
    links = $linksText
    status_text = 'ожидает решения.'
    priority = $Priority
    question = $Question
    owner = $Owner
    panel_status = $PanelStatus
}

$registry.decisions = @($decisions)
$registry.updated_real_date = $today

$json = ($registry | ConvertTo-Json -Depth 8).TrimEnd() + "`n"
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($registryPath, $json, $encoding)

Update-CurrentContext -PendingId $Id -PendingQuestion $Question -PendingOwner $Owner -PendingStatus $PanelStatus

& (Join-Path $root 'tools\Собрать_решения.ps1') -SkipCheck
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

"Created pending decision: $Id"
}
