param(
    [int]$MaxDecisions = 10,
    [int]$MaxQuestions = 8,
    [int]$MaxFronts = 10,
    [int]$MaxTimers = 8,
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
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

    $cells = $Line.Trim('|') -split '\|' | ForEach-Object { $_.Trim() }
    return ,$cells
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
        if ($null -ne $cells -and $cells[0] -notin @('ID', 'Приоритет')) {
            $rows.Add($cells) | Out-Null
        }
    }

    return $rows
}

function Add-Table {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string[]]$Header,
        [object[]]$Rows,
        [string]$EmptyText
    )

    if ($Rows.Count -eq 0) {
        $Lines.Add($EmptyText) | Out-Null
        return
    }

    $Lines.Add('| ' + ($Header -join ' | ') + ' |') | Out-Null
    $Lines.Add('| ' + (($Header | ForEach-Object { '---' }) -join ' | ') + ' |') | Out-Null
    foreach ($row in $Rows) {
        $Lines.Add('| ' + ($row -join ' | ') + ' |') | Out-Null
    }
}

$openQuestionsPath = Join-Path $root '01_Кампания\03_Нерешенные_вопросы.md'
$frontTrackerPath = Join-Path $root '01_Кампания\06_Фронты_и_таймеры.md'
$targetPath = Join-Path $root '01_Кампания\07_Следующий_ход.md'

$openQuestions = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
$frontTracker = Get-Content -Raw -Encoding UTF8 -LiteralPath $frontTrackerPath
$today = Get-Date -Format 'yyyy-MM-dd'

$decisionRows = Get-TableRows -Text $openQuestions -Heading 'Активные решения' |
    Where-Object { $_.Count -ge 5 -and $_[4] -eq 'active' } |
    Select-Object -First $MaxDecisions

$questionRows = Get-TableRows -Text $openQuestions -Heading 'Вопросы главы 2' |
    Where-Object { $_.Count -ge 5 -and $_[4] -eq 'active' } |
    Select-Object -First $MaxQuestions

$frontRows = Get-TableRows -Text $frontTracker -Heading 'Срочные развилки' |
    Where-Object { $_.Count -ge 6 -and $_[1] -in @('критический', 'высокий') } |
    Select-Object -First $MaxFronts

$timerRows = Get-TableRows -Text $frontTracker -Heading 'Таймеры угроз' |
    Where-Object { $_.Count -ge 4 -and ($_[2] -match 'актив|критич') } |
    Select-Object -First $MaxTimers

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Следующий ход') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: next_turn_panel') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: support') | Out-Null
$lines.Add('current_chapter: 2') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_панель_хода.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл пересобирается из `01_Кампания/03_Нерешенные_вопросы.md` и `01_Кампания/06_Фронты_и_таймеры.md`; вопросы и фронты в свою очередь собираются из JSON-реестров в `09_Реестры`. Не веди его вручную, если можно запустить `.\tools\Собрать_панель_хода.ps1`.') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('## Ближайшие решения') | Out-Null
$lines.Add('') | Out-Null
Add-Table -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Владелец / ветка', 'Статус') -Rows $decisionRows -EmptyText 'Нет активных pending-решений.'
$lines.Add('') | Out-Null
$lines.Add('## Активные вопросы') | Out-Null
$lines.Add('') | Out-Null
Add-Table -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Владелец / ветка', 'Статус') -Rows $questionRows -EmptyText 'Нет активных вопросов главы со статусом `active`.'
$lines.Add('') | Out-Null
$lines.Add('## Срочные фронты') | Out-Null
$lines.Add('') | Out-Null
Add-Table -Lines $lines -Header @('FRONT-ID', 'Приоритет', 'Фронт', 'Суть', 'Следующий триггер', 'Связанные файлы') -Rows $frontRows -EmptyText 'Нет срочных фронтов.'
$lines.Add('') | Out-Null
$lines.Add('## Активные таймеры') | Out-Null
$lines.Add('') | Out-Null
Add-Table -Lines $lines -Header @('FRONT-ID', 'Таймер', 'Статус', 'Что считать срабатыванием') -Rows $timerRows -EmptyText 'Нет активных таймеров.'
$lines.Add('') | Out-Null
$lines.Add('## Перед ответом') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('1. Если ход стал каноном, обнови связанные сцены, решения, вопросы и фронты.') | Out-Null
$lines.Add('2. После сюжетного обновления пересобери этот файл командой `.\tools\Собрать_панель_хода.ps1`.') | Out-Null
$lines.Add('3. Затем запусти `.\tools\Проверить_проект.ps1`.') | Out-Null

$panelText = ($lines -join "`n").TrimEnd() + "`n"
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($targetPath, $panelText, $utf8NoBom)

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Updated next turn panel: 01_Кампания/07_Следующий_ход.md"
}
