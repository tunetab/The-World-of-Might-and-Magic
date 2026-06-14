param(
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$registryDir = Join-Path $root '09_Реестры'
$decisionRegistryPath = Join-Path $registryDir 'Решения.json'
$questionRegistryPath = Join-Path $registryDir 'Вопросы.json'

$pendingDecisionPath = Join-Path $registryDir 'Решения_незакрытые.json'
$acceptedDecisionPath = Join-Path $registryDir 'Решения_закрытые.json'
$openQuestionPath = Join-Path $registryDir 'Вопросы_открытые.json'
$closedQuestionPath = Join-Path $registryDir 'Вопросы_закрытые.json'

function Read-JsonFile {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Registry file is missing: $Path"
    }

    return (Get-Content -Raw -Encoding UTF8 -LiteralPath $Path) | ConvertFrom-Json
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Text
    )

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Save-JsonFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $json = $Value | ConvertTo-Json -Depth 12
    Write-Utf8NoBom -Path $Path -Text ($json.TrimEnd() + "`n")
}

function New-StatusCounts {
    param(
        [object[]]$Items,
        [string[]]$Statuses
    )

    $counts = [ordered]@{
        total = @($Items).Count
    }

    foreach ($status in $Statuses) {
        $counts[$status] = @($Items | Where-Object { $_.status -eq $status }).Count
    }

    return [pscustomobject]$counts
}

function New-StateCounts {
    param(
        [object[]]$Items,
        [string[]]$States
    )

    $counts = [ordered]@{
        total = @($Items).Count
    }

    foreach ($state in $States) {
        $counts[$state] = @($Items | Where-Object { $_.state -eq $state }).Count
    }

    return [pscustomobject]$counts
}

function New-ClosedQuestionRows {
    param(
        [object[]]$Questions,
        [object[]]$History
    )

    $historyById = @{}
    foreach ($item in $History) {
        if (-not [string]::IsNullOrWhiteSpace([string]$item.id)) {
            $historyById[[string]$item.id] = $item
        }
    }

    $rows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($question in $Questions) {
        $historyItem = $null
        if ($historyById.ContainsKey([string]$question.id)) {
            $historyItem = $historyById[[string]$question.id]
        }

        $rows.Add([pscustomobject][ordered]@{
            id = $question.id
            priority = $question.priority
            text = $question.text
            owner = $question.owner
            scope = $question.scope
            status = $question.status
            resolved_real_date = if ($historyItem) { $historyItem.date } else { $null }
            resolution = if ($historyItem) { $historyItem.resolution } else { $null }
        }) | Out-Null
    }

    return @($rows.ToArray())
}

$decisionRegistry = Read-JsonFile -Path $decisionRegistryPath
$questionRegistry = Read-JsonFile -Path $questionRegistryPath

$decisions = @($decisionRegistry.decisions)
$pendingDecisions = @($decisions | Where-Object { $_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*' })
$acceptedDecisions = @($decisions | Where-Object { $_.state -eq 'accepted' -and $_.id -match '^DEC-\d{3}$' })

$questions = @($questionRegistry.questions)
$history = @($questionRegistry.history)
$openQuestions = @($questions | Where-Object { $_.status -ne 'resolved' })
$closedQuestions = @($questions | Where-Object { $_.status -eq 'resolved' })

$pendingDecisionView = [pscustomobject][ordered]@{
    type = 'pending_decision_view'
    status = 'active'
    canon_level = 'support'
    updated_real_date = $today
    generated_from = @('09_Реестры/Решения.json')
    count = $pendingDecisions.Count
    counts = New-StateCounts -Items $pendingDecisions -States @('pending')
    decisions = @($pendingDecisions)
}

$acceptedDecisionView = [pscustomobject][ordered]@{
    type = 'accepted_decision_view'
    status = 'active'
    canon_level = 'support'
    updated_real_date = $today
    generated_from = @('09_Реестры/Решения.json')
    count = $acceptedDecisions.Count
    counts = New-StateCounts -Items $acceptedDecisions -States @('accepted')
    decisions = @($acceptedDecisions)
}

$openQuestionView = [pscustomobject][ordered]@{
    type = 'open_question_view'
    status = 'active'
    canon_level = 'support'
    current_chapter = $questionRegistry.current_chapter
    updated_real_date = $today
    generated_from = @('09_Реестры/Вопросы.json')
    count = $openQuestions.Count
    counts = New-StatusCounts -Items $openQuestions -Statuses @('active', 'waiting', 'later')
    questions = @($openQuestions)
}

$closedQuestionRows = New-ClosedQuestionRows -Questions $closedQuestions -History $history
$closedQuestionView = [pscustomobject][ordered]@{
    type = 'closed_question_view'
    status = 'active'
    canon_level = 'support'
    current_chapter = $questionRegistry.current_chapter
    updated_real_date = $today
    generated_from = @('09_Реестры/Вопросы.json')
    count = $closedQuestionRows.Count
    counts = New-StatusCounts -Items $closedQuestions -Statuses @('resolved')
    questions = @($closedQuestionRows)
    history = @($history)
}

Save-JsonFile -Path $pendingDecisionPath -Value $pendingDecisionView
Save-JsonFile -Path $acceptedDecisionPath -Value $acceptedDecisionView
Save-JsonFile -Path $openQuestionPath -Value $openQuestionView
Save-JsonFile -Path $closedQuestionPath -Value $closedQuestionView

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_реестры.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Updated registry JSON views: 09_Реестры/Решения_незакрытые.json, 09_Реестры/Решения_закрытые.json, 09_Реестры/Вопросы_открытые.json, 09_Реестры/Вопросы_закрытые.json"
}
