[CmdletBinding()]
param(
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$frontTrackerPath = Join-Path $root '01_Кампания\06_Фронты_и_таймеры.md'

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

function Add-Table {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string[]]$Header,
        [object[]]$Rows,
        [scriptblock]$RenderRow
    )

    $Lines.Add(('| ' + ($Header -join ' | ') + ' |'))
    $Lines.Add(('| ' + (($Header | ForEach-Object { '---' }) -join ' | ') + ' |'))
    foreach ($row in $Rows) {
        $Lines.Add((& $RenderRow $row))
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

    $lines.Add('# Фронты и таймеры')
    $lines.Add('')
    $lines.Add('---')
    $lines.Add('type: front_tracker')
    $lines.Add('status: active')
    $lines.Add('canon_level: active')
    $lines.Add("current_chapter: $($Registry.current_chapter)")
    $lines.Add("date_in_story: $($Registry.date_in_story)")
    $lines.Add("updated_real_date: $($Registry.updated_real_date)")
    $lines.Add('generated_by: tools/Собрать_фронты.ps1')
    $lines.Add('source_registry: 09_Реестры/Фронты.json')
    $lines.Add('---')
    $lines.Add('')
    $lines.Add('Этот файл пересобирается из `09_Реестры/Фронты.json`. Для создания фронта используй `.\tools\Новый_фронт.ps1`, для обновления фронта или таймера - `.\tools\Обновить_фронт.ps1`.')
    $lines.Add('')
    $lines.Add('Главная точка текущей главы остается в `01_Кампания/01_Активная_глава.md`. Здесь фиксируются не новые события, а удобная карта уже зафиксированных фронтов и ожидаемых триггеров.')
    $lines.Add('')
    $lines.Add('## Справочник FRONT-ID')
    $lines.Add('')
    Add-Table -Lines $lines -Header @('ID', 'Фронт') -Rows $fronts -RenderRow {
        param($row)
        '| ' + (@(
            Format-WmmaTableCell $row.id
            Format-WmmaTableCell $row.name
        ) -join ' | ') + ' |'
    }
    $lines.Add('')
    $lines.Add('## Срочные развилки')
    $lines.Add('')
    Add-Table -Lines $lines -Header @('ID', 'Приоритет', 'Фронт', 'Суть', 'Следующий триггер', 'Связанные файлы') -Rows $urgentForks -RenderRow {
        param($row)
        '| ' + (@(
            Format-WmmaTableCell $row.id
            Format-WmmaTableCell $row.priority
            Format-WmmaTableCell $row.front
            Format-WmmaTableCell $row.summary
            Format-WmmaTableCell $row.trigger
            Format-LinksCell @($row.links)
        ) -join ' | ') + ' |'
    }
    $lines.Add('')
    $lines.Add('## Активные фронты')
    $lines.Add('')
    Add-Table -Lines $lines -Header @('ID', 'Фронт', 'Контроль / участники', 'Текущее состояние', 'Риск', 'Следующий триггер') -Rows $activeFronts -RenderRow {
        param($row)
        '| ' + (@(
            Format-WmmaTableCell $row.id
            Format-WmmaTableCell $row.front
            Format-WmmaTableCell $row.participants
            Format-WmmaTableCell $row.state
            Format-WmmaTableCell $row.risk
            Format-WmmaTableCell $row.next_trigger
        ) -join ' | ') + ' |'
    }
    $lines.Add('')
    $lines.Add('## Таймеры угроз')
    $lines.Add('')
    Add-Table -Lines $lines -Header @('ID', 'Таймер', 'Статус', 'Что считать срабатыванием') -Rows $timers -RenderRow {
        param($row)
        '| ' + (@(
            Format-WmmaTableCell $row.id
            Format-WmmaTableCell $row.timer
            Format-WmmaTableCell $row.status
            Format-WmmaTableCell $row.trigger
        ) -join ' | ') + ' |'
    }
    $lines.Add('')
    $lines.Add('## Правило обновления')
    $lines.Add('')
    $lines.Add('После каждого крупного сюжетного апдейта текущей кампании обновлять:')
    $lines.Add('')
    for ($i = 0; $i -lt $rules.Count; $i++) {
        $lines.Add(('{0}. {1}' -f ($i + 1), $rules[$i]))
    }

    Write-Utf8NoBom -Path $frontTrackerPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

$registry = Read-WmmaRegistry $root 'Фронты'

Render-FrontTracker -Registry $registry

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
