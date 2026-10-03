param([switch]$SkipCheck,[object]$Data)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $Data=Resolve-WmmaReadModel $root $Data
    $state=$Data.state;$knowledge=$Data.knowledge
    $decisions=$Data.decisions;$questions=$Data.questions
    $chapter=$Data.chapter;$byId=$Data.entities_by_id
    $lines=[Collections.Generic.List[string]]::new()
    $lines.Add('# Текущий контекст кампании'); $lines.Add(''); $lines.Add('---')
    $lines.Add('type: ai_current_context');$lines.Add('status: active');$lines.Add('canon_level: support');$lines.Add("current_chapter: $chapter");$lines.Add('generated_by: tools/Собрать_контекст.ps1');$lines.Add('---');$lines.Add('')
    $lines.Add('Короткий вход в кампанию. Основания хранятся в реестрах Контекст и Знания. Для подробностей используйте Получить_контекст с веткой; большие хроники читаются по ссылкам.');$lines.Add('')
    $lines.Add('## Текущая точка');$lines.Add('')
    foreach($fact in $knowledge.facts | Where-Object {$state.fact_ids -contains $_.id}) { $lines.Add("- [$($fact.truth)] $($fact.text)") }
    $lines.Add('');$lines.Add('## Ветки');$lines.Add('')
    foreach($branch in $state.branches) { $lines.Add("- $($branch.name): $($branch.situation) ``01_Кампания/Контекст/$($branch.name).md``") }
    $lines.Add('');$lines.Add('## Немедленные решения');$lines.Add('')
    $i=1
    foreach($d in $decisions.decisions | Where-Object {$_.state -eq 'pending'} | Sort-Object @{Expression={Get-WmmaPriority $_.priority};Descending=$true},id){
        $lines.Add(('{0}. `{1}` — {2} [{3}]' -f $i,$d.id,$d.question,$d.panel_status));$i++
    }
    $lines.Add('');$lines.Add('## Ограничения');$lines.Add('')
    $lines.Add('- Не смешивать факт мира, чужое заявление и знание персонажа. Сон и легенда не подтверждают свои объяснения.')
    $lines.Add('- Не придумывать численность, бюджеты, сроки, свойства артефактов и решения игрока.')
    $lines.Add('- Михаэль и Харагорн — разные персонажи. Проверять словарь имён при неоднозначности.')
    $lines.Add('- Закрытые главы сохраняются; противоречия сначала сверять с источниками. Технические изменения не являются сюжетом.')
    Write-WmmaText (Join-Path $root '01_Кампания/00_Текущий_контекст.md') (($lines -join "`n")+"`n")
    foreach($branch in $state.branches){
        $packet=Get-WmmaContext -Root $root -Branch $branch.name -MaxWords 1800 -Data $Data
        Write-WmmaText (Join-Path $root "01_Кампания/Контекст/$($branch.name).md") $packet.text
        $profile=@("# Ветка $($branch.name)",'','---','type: character_branch',"main_character: $($byId[$branch.character_id].name)",'status: active','canon_level: support',"last_closed_chapter: $($chapter-1)","current_chapter: $chapter",'generated_by: tools/Собрать_контекст.ps1','---','','## Текущее положение','',$branch.situation,'','## Последняя сцена','')
        foreach($id in $branch.scene_ids) { $profile += '- ' + $byId[$id].name + ': `' + $byId[$id].path + '`.' }
        $profile += @('','## Открытые последствия','')
        foreach($d in $decisions.decisions | Where-Object {$_.state -eq 'pending' -and $branch.focus_ids -contains $_.id}){$profile += '- ' + $d.id + ': ' + $d.question}
        foreach($q in $questions.questions | Where-Object {$_.status -ne 'resolved' -and $branch.focus_ids -contains $_.id}){$profile += '- ' + $q.id + ': ' + $q.text}
        $profile += @('','## История','',('Подробности прежних глав: `' + $branch.history + '`. Текущий пакет: `01_Кампания/Контекст/' + $branch.name + '.md`.'))
        Write-WmmaText (Join-Path $root "01_Кампания/Ветки/$($branch.name)/00_Профиль_ветки.md") (($profile -join "`n")+"`n")
    }
    $summary=@('# Сводка кампании','','---','type: campaign_summary','status: active','canon_level: support',"last_closed_chapter: $($chapter-1)","current_chapter: $chapter",'generated_by: tools/Собрать_контекст.ps1','---','','## Текущее положение','','Актуальная точка: `01_Кампания/00_Текущий_контекст.md`. Ближайшие вопросы и угрозы: `01_Кампания/07_Следующий_ход.md`.','','## История кампании','','Полная прежняя сводка сохранена в `01_Кампания/История/Сводка_до_v2.md`. Завершённые главы и их итоги находятся в папках глав и архива канона.','','## Активные линии','')
    foreach($branch in $state.branches){$summary += '- ' + $branch.name + ': ' + $branch.situation}
    Write-WmmaText (Join-Path $root '01_Кампания/00_Сводка_кампании.md') (($summary -join "`n")+"`n")
    $world=@('# Состояние мира','','---','type: world_state','status: active','canon_level: support',"current_chapter: $chapter",'generated_by: tools/Собрать_контекст.ps1','---','','## Новейшее развитие','')
    foreach($f in $knowledge.facts | Where-Object {$state.fact_ids -contains $_.id}){$world += '- ' + $f.text}
    $world += @('','## Политика, война и экономика','','Текущие фронты, их состояние и следующие триггеры: `01_Кампания/06_Фронты_и_таймеры.md`. Активы принадлежат карточкам владельцев; точные численности не выводятся из иллюстраций и прежних ведомостей.','','## Исторические состояния','','Полный прежний документ: `01_Кампания/История/Состояние_мира_до_v2.md`. Его старые текущие формулировки не считаются сегодняшним состоянием.','')
    Write-WmmaText (Join-Path $root '01_Кампания/05_Состояние_мира.md') (($world -join "`n").TrimEnd()+"`n")
    'Current context, branch packets and historical navigation rebuilt.'
    if(-not $SkipCheck){& (Join-Path $root 'tools/Проверить_контекст.ps1')}
}
