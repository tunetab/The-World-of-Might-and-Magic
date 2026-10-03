param([int]$MaxDecisions=10,[int]$MaxQuestions=8,[int]$MaxFronts=10,[int]$MaxTimers=8,[string]$Branch='',[switch]$SkipCheck,[object]$Data)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $Data=Resolve-WmmaReadModel $root $Data
    $selection=Get-WmmaSelection -Root $root -Branch $Branch -Data $Data
    $lines=[Collections.Generic.List[string]]::new()
    $lines.Add('# Следующий ход'); $lines.Add(''); $lines.Add('---'); $lines.Add('type: next_turn_panel'); $lines.Add('status: active'); $lines.Add('canon_level: support')
    $lines.Add("current_chapter: $($Data.chapter)"); $lines.Add('generated_by: tools/Собрать_панель_хода.ps1'); $lines.Add('---'); $lines.Add('')
    $lines.Add('Собирается напрямую из реестров решений, вопросов, фронтов и контекста. Порядок: связь с текущим ходом, ветка, приоритет, стабильный ID. Вопросы прошлых глав сохраняют исходные ID.'); $lines.Add('')
    function Add-PanelTable([string]$Heading,[string[]]$Header,[object[]]$Rows) {
        $lines.Add("## $Heading"); $lines.Add('')
        $lines.Add('| ' + ($Header -join ' | ') + ' |'); $lines.Add('| ' + (($Header | ForEach-Object {'---'}) -join ' | ') + ' |')
        foreach ($row in $Rows) { $lines.Add('| ' + (@($row | ForEach-Object {Convert-WmmaCell $_}) -join ' | ') + ' |') }
        $lines.Add('')
    }
    $rows=@(); foreach($d in @($selection.decisions | Select-Object -First $MaxDecisions)) { $rows+=,@($d.id,$d.priority,$d.question,$d.owner,$d.panel_status) }
    Add-PanelTable 'Ближайшие решения' @('ID','Приоритет','Вопрос','Владелец / ветка','Статус') $rows
    $rows=@(); foreach($q in @($selection.questions | Select-Object -First $MaxQuestions)) { $rows+=,@($q.id,$q.priority,$q.text,$q.owner,$q.status) }
    Add-PanelTable 'Активные вопросы' @('ID','Приоритет','Вопрос','Владелец / ветка','Статус') $rows
    $rows=@(); foreach($f in @($selection.fronts | Select-Object -First $MaxFronts)) { $rows+=,@($f.id,$f.priority,$f.front,$f.summary,$f.trigger) }
    Add-PanelTable 'Срочные фронты' @('FRONT-ID','Приоритет','Фронт','Суть','Следующий триггер') $rows
    $rows=@(); foreach($t in @($selection.timers | Select-Object -First $MaxTimers)) { $rows+=,@($t.id,$t.timer,$t.status,$t.trigger) }
    Add-PanelTable 'Активные таймеры' @('FRONT-ID','Таймер','Статус','Что считать срабатыванием') $rows
    $lines.Add("Всего активных вопросов: $($selection.questions.Count); показано: $([Math]::Min($MaxQuestions,$selection.questions.Count)). Остальные доступны в реестре и через Получить_контекст с указанием ветки или запроса.")
    Write-WmmaText (Join-Path $root '01_Кампания/07_Следующий_ход.md') (($lines -join "`n")+"`n")
    if (-not $SkipCheck) { & (Join-Path $root 'tools/Проверить_проект.ps1') }
    'Updated next turn panel: 01_Кампания/07_Следующий_ход.md'
}
