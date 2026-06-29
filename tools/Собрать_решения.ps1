param(
    [switch]$ImportFromMarkdown,

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
$registryPath = Join-Path $registryDir 'Решения.json'
$decisionLogPath = Join-Path $root '01_Кампания\02_Журнал_решений.md'
$openQuestionsPath = Join-Path $root '01_Кампания\03_Нерешенные_вопросы.md'

function Read-Text {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }

    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Text
    )

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-SectionText {
    param(
        [string]$Text,
        [string]$Heading
    )

    $escapedHeading = [regex]::Escape($Heading)
    if ($Text -match "(?ms)^##\s+$escapedHeading\s*\r?\n(.+?)(?:\r?\n##\s+|\z)") {
        return $Matches[1]
    }

    return ''
}

function Convert-MarkdownTableRow {
    param([string]$Line)

    if ($Line -notmatch '^\|.+\|$' -or $Line -match '^\|\s*-') {
        return $null
    }

    return ,($Line.Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
}

function Get-PendingRowsById {
    $rowsById = @{}
    $openQuestions = Read-Text -Path $openQuestionsPath
    $section = Get-SectionText -Text $openQuestions -Heading 'Активные решения'

    foreach ($line in ($section -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -eq $cells -or $cells.Count -lt 5 -or $cells[0] -notmatch '^DEC-PENDING-\d{3}$') {
            continue
        }

        $rowsById[$cells[0]] = [pscustomobject][ordered]@{
            priority = $cells[1]
            question = $cells[2]
            owner = $cells[3]
            panel_status = $cells[4]
        }
    }

    return $rowsById
}

function Get-DecisionField {
    param(
        [string]$Body,
        [string]$Name
    )

    $escapedName = [regex]::Escape($Name)
    if ($Body -match "(?m)^$escapedName\s*:\s*(.*?)\s*$") {
        return $Matches[1].Trim()
    }

    return ''
}

function Import-DecisionRegistry {
    $decisionLog = Read-Text -Path $decisionLogPath
    $pendingRowsById = Get-PendingRowsById
    $decisions = New-Object 'System.Collections.Generic.List[object]'
    $matches = [regex]::Matches(
        $decisionLog,
        '(?ms)^###\s+(DEC(?:-PENDING)?-\d{3})\s*\r?\n(.*?)(?=^###\s+|^##\s+|\z)'
    )

    foreach ($match in $matches) {
        $id = $match.Groups[1].Value.Trim()
        $body = $match.Groups[2].Value
        $pendingRow = $null
        if ($pendingRowsById.ContainsKey($id)) {
            $pendingRow = $pendingRowsById[$id]
        }

        $state = if ($id -like 'DEC-PENDING-*') { 'pending' } else { 'accepted' }
        $decisions.Add([pscustomobject][ordered]@{
            id = $id
            state = $state
            real_date = Get-DecisionField -Body $body -Name 'Дата в реальности'
            story_date = Get-DecisionField -Body $body -Name 'Дата в сюжете'
            player_character = Get-DecisionField -Body $body -Name 'Игрок / персонаж'
            scene = Get-DecisionField -Body $body -Name 'Сцена'
            choice = Get-DecisionField -Body $body -Name 'Выбор'
            player_addition = Get-DecisionField -Body $body -Name 'Дополнение игрока'
            immediate_effect = Get-DecisionField -Body $body -Name 'Немедленный эффект'
            long_term_consequences = Get-DecisionField -Body $body -Name 'Долгосрочные последствия'
            links = Get-DecisionField -Body $body -Name 'Связанные файлы'
            status_text = Get-DecisionField -Body $body -Name 'Статус'
            priority = if ($pendingRow) { $pendingRow.priority } else { $null }
            question = if ($pendingRow) { $pendingRow.question } else { $null }
            owner = if ($pendingRow) { $pendingRow.owner } else { $null }
            panel_status = if ($pendingRow) { $pendingRow.panel_status } else { $null }
        }) | Out-Null
    }

    return [pscustomobject][ordered]@{
        type = 'decision_registry'
        status = 'active'
        canon_level = 'support'
        updated_real_date = $today
        generated_from = @(
            '01_Кампания/02_Журнал_решений.md',
            '01_Кампания/03_Нерешенные_вопросы.md'
        )
        decisions = @($decisions.ToArray())
    }
}

function Read-DecisionRegistry {
    if (-not (Test-Path -LiteralPath $registryPath)) {
        throw "Decision registry is missing: 09_Реестры/Решения.json. Run .\tools\Собрать_решения.ps1 -ImportFromMarkdown once."
    }

    $json = Read-Text -Path $registryPath
    return $json | ConvertFrom-Json
}

function Save-DecisionRegistry {
    param([object]$Registry)

    if (-not (Test-Path -LiteralPath $registryDir)) {
        New-Item -ItemType Directory -Path $registryDir -Force | Out-Null
    }

    $json = ($Registry | ConvertTo-Json -Depth 8)
    Write-Utf8NoBom -Path $registryPath -Text ($json.TrimEnd() + "`n")
}

function Format-Value {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return ''
    }

    return (($Value.ToString() -replace '\r?\n', ' ').Trim())
}

function Add-RawLine {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Line = ''
    )

    $Lines.Add($Line) | Out-Null
}

function Add-DecisionBlock {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [object]$Decision
    )

    Add-RawLine -Lines $Lines -Line "### $($Decision.id)"
    Add-RawLine -Lines $Lines
    Add-RawLine -Lines $Lines -Line "Дата в реальности: $(Format-Value $Decision.real_date)"
    Add-RawLine -Lines $Lines -Line "Дата в сюжете: $(Format-Value $Decision.story_date)"
    Add-RawLine -Lines $Lines -Line "Игрок / персонаж: $(Format-Value $Decision.player_character)"
    Add-RawLine -Lines $Lines -Line "Сцена: $(Format-Value $Decision.scene)"
    Add-RawLine -Lines $Lines -Line "Выбор: $(Format-Value $Decision.choice)"
    Add-RawLine -Lines $Lines -Line "Дополнение игрока: $(Format-Value $Decision.player_addition)"
    Add-RawLine -Lines $Lines -Line "Немедленный эффект: $(Format-Value $Decision.immediate_effect)"
    Add-RawLine -Lines $Lines -Line "Долгосрочные последствия: $(Format-Value $Decision.long_term_consequences)"
    Add-RawLine -Lines $Lines -Line "Связанные файлы: $(Format-Value $Decision.links)"
    Add-RawLine -Lines $Lines -Line "Статус: $(Format-Value $Decision.status_text)"
}

function Render-DecisionLog {
    param([object]$Registry)

    $decisions = @($Registry.decisions)
    $pendingDecisions = @(
        $decisions |
            Where-Object { $_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*' } |
            Sort-Object @{ Expression = { [int]($_.id -replace '^DEC-PENDING-', '') } }
    )
    $acceptedDecisions = @(
        $decisions |
            Where-Object { $_.state -eq 'accepted' -and $_.id -match '^DEC-\d{3}$' } |
            Sort-Object @{ Expression = { [int]($_.id -replace '^DEC-', '') } }
    )
    $lines = New-Object 'System.Collections.Generic.List[string]'

    Add-RawLine -Lines $lines -Line '# Журнал решений'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines -Line 'type: decision_log'
    Add-RawLine -Lines $lines -Line 'status: active'
    Add-RawLine -Lines $lines -Line 'canon_level: active'
    Add-RawLine -Lines $lines -Line "updated_real_date: $($Registry.updated_real_date)"
    Add-RawLine -Lines $lines -Line 'generated_by: tools/Собрать_решения.ps1'
    Add-RawLine -Lines $lines -Line 'source_registry: 09_Реестры/Решения.json'
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Этот файл пересобирается из `09_Реестры/Решения.json`. Для создания `DEC-PENDING-*` используй `.\tools\Новое_решение.ps1`, для закрытия - `.\tools\Закрыть_решение.ps1`.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Формат записи'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '```text'
    Add-RawLine -Lines $lines -Line 'ID:'
    Add-RawLine -Lines $lines -Line 'Дата в реальности:'
    Add-RawLine -Lines $lines -Line 'Дата в сюжете:'
    Add-RawLine -Lines $lines -Line 'Игрок / персонаж:'
    Add-RawLine -Lines $lines -Line 'Сцена:'
    Add-RawLine -Lines $lines -Line 'Выбор:'
    Add-RawLine -Lines $lines -Line 'Дополнение игрока:'
    Add-RawLine -Lines $lines -Line 'Немедленный эффект:'
    Add-RawLine -Lines $lines -Line 'Долгосрочные последствия:'
    Add-RawLine -Lines $lines -Line 'Связанные файлы:'
    Add-RawLine -Lines $lines -Line 'Статус:'
    Add-RawLine -Lines $lines -Line '```'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Ожидают решения'
    foreach ($decision in $pendingDecisions) {
        Add-RawLine -Lines $lines
        Add-DecisionBlock -Lines $lines -Decision $decision
    }

    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Принятые решения'
    foreach ($decision in $acceptedDecisions) {
        Add-RawLine -Lines $lines
        Add-DecisionBlock -Lines $lines -Decision $decision
    }

    Write-Utf8NoBom -Path $decisionLogPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

if ($ImportFromMarkdown) {
    $registry = Import-DecisionRegistry
    Save-DecisionRegistry -Registry $registry
} else {
    $registry = Read-DecisionRegistry
}

$registry.updated_real_date = $today
Render-DecisionLog -Registry $registry
Save-DecisionRegistry -Registry $registry

if (Test-Path -LiteralPath (Join-Path $root 'tools\Собрать_вопросы.ps1')) {
    & (Join-Path $root 'tools\Собрать_вопросы.ps1') -SkipCheck
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
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

"Updated decision registry and Markdown log: 09_Реестры/Решения.json"
}
