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
$registryPath = Join-Path $registryDir 'Фронты.json'
$frontTrackerPath = Join-Path $root '01_Кампания\06_Фронты_и_таймеры.md'

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

function Get-MetaField {
    param(
        [string]$Text,
        [string]$Field
    )

    if ($Text -match '(?s)^# .+?\r?\n\r?\n---\r?\n(.+?)\r?\n---') {
        $meta = $Matches[1]
        $escapedField = [regex]::Escape($Field)
        if ($meta -match "(?m)^$escapedField\s*:\s*(.+?)\s*$") {
            return $Matches[1].Trim()
        }
    }

    return $null
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

function Get-TableRows {
    param(
        [string]$Text,
        [string]$Heading
    )

    $section = Get-SectionText -Text $Text -Heading $Heading
    $rows = New-Object 'System.Collections.Generic.List[object]'

    foreach ($line in ($section -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -ne $cells -and $cells[0] -notin @('ID', 'FRONT-ID')) {
            $rows.Add($cells) | Out-Null
        }
    }

    return $rows
}

function ConvertTo-LinkArray {
    param([string]$Cell)

    if ([string]::IsNullOrWhiteSpace($Cell) -or $Cell.Trim() -eq '-') {
        return @()
    }

    $matches = [regex]::Matches($Cell, '`([^`]+)`')
    if ($matches.Count -gt 0) {
        return @($matches | ForEach-Object { $_.Groups[1].Value.Trim() })
    }

    return @($Cell.Trim())
}

function Get-RuleLines {
    param([string]$FrontTracker)

    $rules = New-Object 'System.Collections.Generic.List[string]'
    $section = Get-SectionText -Text $FrontTracker -Heading 'Правило обновления'
    foreach ($line in ($section -split "\r?\n")) {
        if ($line -match '^\d+\.\s+(.+?)\s*$') {
            $rules.Add($Matches[1].Trim()) | Out-Null
        }
    }

    if ($rules.Count -eq 0) {
        foreach ($rule in @(
            'строку соответствующего фронта;',
            'статус таймера, если событие сдвинуло угрозу;',
            '`01_Кампания/03_Нерешенные_вопросы.md`, если появился новый открытый вопрос;',
            '`01_Кампания/03_Закрытые_вопросы.md`, если вопрос закрыт;',
            '`01_Кампания/05_Состояние_мира.md`, если изменился баланс мира.'
        )) {
            $rules.Add($rule) | Out-Null
        }
    }

    return $rules
}

function Import-FrontRegistry {
    $frontTracker = Read-Text -Path $frontTrackerPath
    $fronts = New-Object 'System.Collections.Generic.List[object]'
    $urgentForks = New-Object 'System.Collections.Generic.List[object]'
    $activeFronts = New-Object 'System.Collections.Generic.List[object]'
    $timers = New-Object 'System.Collections.Generic.List[object]'

    foreach ($row in (Get-TableRows -Text $frontTracker -Heading 'Справочник FRONT-ID')) {
        if ($row.Count -lt 2) {
            continue
        }

        $fronts.Add([pscustomobject][ordered]@{
            id = $row[0]
            name = $row[1]
        }) | Out-Null
    }

    foreach ($row in (Get-TableRows -Text $frontTracker -Heading 'Срочные развилки')) {
        if ($row.Count -lt 6) {
            continue
        }

        $urgentForks.Add([pscustomobject][ordered]@{
            id = $row[0]
            priority = $row[1]
            front = $row[2]
            summary = $row[3]
            trigger = $row[4]
            links = @(ConvertTo-LinkArray -Cell $row[5])
        }) | Out-Null
    }

    foreach ($row in (Get-TableRows -Text $frontTracker -Heading 'Активные фронты')) {
        if ($row.Count -lt 6) {
            continue
        }

        $activeFronts.Add([pscustomobject][ordered]@{
            id = $row[0]
            front = $row[1]
            participants = $row[2]
            state = $row[3]
            risk = $row[4]
            next_trigger = $row[5]
        }) | Out-Null
    }

    foreach ($row in (Get-TableRows -Text $frontTracker -Heading 'Таймеры угроз')) {
        if ($row.Count -lt 4) {
            continue
        }

        $timers.Add([pscustomobject][ordered]@{
            id = $row[0]
            timer = $row[1]
            status = $row[2]
            trigger = $row[3]
        }) | Out-Null
    }

    $ruleLines = @(Get-RuleLines -FrontTracker $frontTracker)

    return [pscustomobject][ordered]@{
        type = 'front_registry'
        status = 'active'
        canon_level = 'support'
        current_chapter = [int](Get-MetaField -Text $frontTracker -Field 'current_chapter')
        date_in_story = Get-MetaField -Text $frontTracker -Field 'date_in_story'
        updated_real_date = $today
        generated_from = @('01_Кампания/06_Фронты_и_таймеры.md')
        fronts = @($fronts.ToArray())
        urgent_forks = @($urgentForks.ToArray())
        active_fronts = @($activeFronts.ToArray())
        timers = @($timers.ToArray())
        rules = @($ruleLines)
    }
}

function Read-FrontRegistry {
    if (-not (Test-Path -LiteralPath $registryPath)) {
        throw "Front registry is missing: 09_Реестры/Фронты.json. Run .\tools\Собрать_фронты.ps1 -ImportFromMarkdown once."
    }

    return (Read-Text -Path $registryPath) | ConvertFrom-Json
}

function Save-FrontRegistry {
    param([object]$Registry)

    if (-not (Test-Path -LiteralPath $registryDir)) {
        New-Item -ItemType Directory -Path $registryDir -Force | Out-Null
    }

    $json = ($Registry | ConvertTo-Json -Depth 8)
    Write-Utf8NoBom -Path $registryPath -Text ($json.TrimEnd() + "`n")
}

function Format-MarkdownCell {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return ''
    }

    return (($Value.ToString() -replace '\|', '/') -replace '\r?\n', ' ').Trim()
}

function Format-ProjectReference {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $reference = $Value.Trim() -replace '\\', '/'
    if ($reference -match '^`.+`$' -or $reference -match '^[a-z]+://') {
        return $reference
    }

    return "``$reference``"
}

function Format-LinksCell {
    param([object[]]$Links)

    $formatted = @(
        @($Links) |
            ForEach-Object { Format-ProjectReference -Value $_ } |
            Where-Object { $_ }
    )

    if ($formatted.Count -eq 0) {
        return '-'
    }

    return ($formatted -join ', ')
}

function Add-RawLine {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Line = ''
    )

    $Lines.Add($Line) | Out-Null
}

function Add-Table {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string[]]$Header,
        [object[]]$Rows,
        [scriptblock]$RenderRow
    )

    Add-RawLine -Lines $Lines -Line ('| ' + ($Header -join ' | ') + ' |')
    Add-RawLine -Lines $Lines -Line ('| ' + (($Header | ForEach-Object { '---' }) -join ' | ') + ' |')
    foreach ($row in $Rows) {
        Add-RawLine -Lines $Lines -Line (& $RenderRow $row)
    }
}

function Render-FrontTracker {
    param([object]$Registry)

    $fronts = @($Registry.fronts)
    $urgentForks = @($Registry.urgent_forks)
    $activeFronts = @($Registry.active_fronts)
    $timers = @($Registry.timers)
    $rules = @($Registry.rules)
    $lines = New-Object 'System.Collections.Generic.List[string]'

    Add-RawLine -Lines $lines -Line '# Фронты и таймеры'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines -Line 'type: front_tracker'
    Add-RawLine -Lines $lines -Line 'status: active'
    Add-RawLine -Lines $lines -Line 'canon_level: active'
    Add-RawLine -Lines $lines -Line "current_chapter: $($Registry.current_chapter)"
    Add-RawLine -Lines $lines -Line "date_in_story: $($Registry.date_in_story)"
    Add-RawLine -Lines $lines -Line "updated_real_date: $($Registry.updated_real_date)"
    Add-RawLine -Lines $lines -Line 'generated_by: tools/Собрать_фронты.ps1'
    Add-RawLine -Lines $lines -Line 'source_registry: 09_Реестры/Фронты.json'
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Этот файл пересобирается из `09_Реестры/Фронты.json`. Для создания фронта используй `.\tools\Новый_фронт.ps1`, для обновления фронта или таймера - `.\tools\Обновить_фронт.ps1`.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Главная точка текущей главы остается в `01_Кампания/01_Активная_глава.md`. Здесь фиксируются не новые события, а удобная карта уже зафиксированных фронтов и ожидаемых триггеров.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Справочник FRONT-ID'
    Add-RawLine -Lines $lines
    Add-Table -Lines $lines -Header @('ID', 'Фронт') -Rows $fronts -RenderRow {
        param($row)
        '| ' + (@(
            Format-MarkdownCell $row.id
            Format-MarkdownCell $row.name
        ) -join ' | ') + ' |'
    }
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Срочные развилки'
    Add-RawLine -Lines $lines
    Add-Table -Lines $lines -Header @('ID', 'Приоритет', 'Фронт', 'Суть', 'Следующий триггер', 'Связанные файлы') -Rows $urgentForks -RenderRow {
        param($row)
        '| ' + (@(
            Format-MarkdownCell $row.id
            Format-MarkdownCell $row.priority
            Format-MarkdownCell $row.front
            Format-MarkdownCell $row.summary
            Format-MarkdownCell $row.trigger
            Format-LinksCell @($row.links)
        ) -join ' | ') + ' |'
    }
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Активные фронты'
    Add-RawLine -Lines $lines
    Add-Table -Lines $lines -Header @('ID', 'Фронт', 'Контроль / участники', 'Текущее состояние', 'Риск', 'Следующий триггер') -Rows $activeFronts -RenderRow {
        param($row)
        '| ' + (@(
            Format-MarkdownCell $row.id
            Format-MarkdownCell $row.front
            Format-MarkdownCell $row.participants
            Format-MarkdownCell $row.state
            Format-MarkdownCell $row.risk
            Format-MarkdownCell $row.next_trigger
        ) -join ' | ') + ' |'
    }
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Таймеры угроз'
    Add-RawLine -Lines $lines
    Add-Table -Lines $lines -Header @('ID', 'Таймер', 'Статус', 'Что считать срабатыванием') -Rows $timers -RenderRow {
        param($row)
        '| ' + (@(
            Format-MarkdownCell $row.id
            Format-MarkdownCell $row.timer
            Format-MarkdownCell $row.status
            Format-MarkdownCell $row.trigger
        ) -join ' | ') + ' |'
    }
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Правило обновления'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'После каждого крупного сюжетного апдейта текущей кампании обновлять:'
    Add-RawLine -Lines $lines
    for ($i = 0; $i -lt $rules.Count; $i++) {
        Add-RawLine -Lines $lines -Line ('{0}. {1}' -f ($i + 1), $rules[$i])
    }

    Write-Utf8NoBom -Path $frontTrackerPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

if ($ImportFromMarkdown) {
    $registry = Import-FrontRegistry
    Save-FrontRegistry -Registry $registry
} else {
    $registry = Read-FrontRegistry
}

$registry.updated_real_date = $today
Render-FrontTracker -Registry $registry
Save-FrontRegistry -Registry $registry

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

"Updated front registry and Markdown tracker: 09_Реестры/Фронты.json"
}
