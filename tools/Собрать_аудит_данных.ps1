param([object]$Data)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $Data=Resolve-WmmaReadModel $root $Data
    $graph=$Data.graph
    $memory=Read-WmmaJson (Join-Path $root '09_Реестры/Память_персонажей.json')
    $knowledge=$Data.knowledge
    $characters=@{};foreach($person in $memory.characters){$characters[$person.character_id]=$person}
    $rows=@($graph.entities|ForEach-Object {
        $person=$characters[$_.id]
        [ordered]@{id=$_.id;type=$_.type;path=$_.path;provenance=$_.provenance;source_ids=@($_.source_ids);historical_source_ids=@($_.historical_source_ids);history_paths=@($_.history_paths);historical_sections=$(if($person){@($person.historical_sections|ForEach-Object {$_.heading})}else{@()});current_position_status=$(if($person){$person.current_position.status}else{$null});voice_status=$(if($person){$person.voice.status}else{$null})}
    })
    $summary=[ordered]@{
        entities=$graph.entities.Count;edges=$graph.edges.Count;references=$graph.references.Count
        original_sources=@($rows|Where-Object provenance -eq original).Count
        linked_to_sources=@($rows|Where-Object provenance -eq linked).Count
        historical_provenance=@($rows|Where-Object provenance -eq historical).Count
        unresolved_provenance=@($rows|Where-Object provenance -eq unresolved).Count
        characters=$memory.characters.Count
        characters_with_historical_sections=@($memory.characters|Where-Object {$_.historical_sections.Count -gt 0}).Count
        current_position_not_recorded=@($memory.characters|Where-Object {$_.current_position.status -eq 'not_recorded'}).Count
        voice_not_established=@($memory.characters|Where-Object {$_.voice.status -eq 'not_established'}).Count
        facts=$knowledge.facts.Count
    }
    Write-WmmaJson (Join-Path $root '10_Обслуживание/Покрытие_данных.json') ([ordered]@{
        schema_version=2;type='data_coverage_view';generated_by='tools/Собрать_аудит_данных.ps1'
        interpretation='Отсутствие прямого источника, текущего положения или манеры речи означает незаполненную связь или незаданный факт. Это не доказательство сюжетной ошибки. Граф показывает ссылки, а не истинность всех утверждений.'
        summary=$summary;entities=$rows
    })
    $lines=@('# Покрытие данных','','---','type: maintenance_report','status: active','canon_level: support','generated_by: tools/Собрать_аудит_данных.ps1','---','',
        'Это автоматически пересобираемый обзор полноты ссылок и памяти. Подробный список каждой сущности находится в [Покрытие_данных.json](Покрытие_данных.json).',
        '',"| Показатель | Количество |",'| --- | ---: |')
    foreach($item in @(
        @('Сущности',$summary.entities),@('Связи участников и источников',$summary.edges),@('Навигационные упоминания',$summary.references),@('Исходные документы',$summary.original_sources),
        @('Сущности с прямой ссылкой на источник',$summary.linked_to_sources),@('Только исторический источник',$summary.historical_provenance),
        @('Без установленной прямой ссылки',$summary.unresolved_provenance),@('Персонажи',$summary.characters),
        @('Персонажи с доступными историческими разделами',$summary.characters_with_historical_sections),
        @('Без раздела о текущем положении',$summary.current_position_not_recorded),@('Без установленной манеры речи',$summary.voice_not_established),
        @('Отдельно размеченные факты',$summary.facts)
    )){$lines+="| $($item[0]) | $($item[1]) |"}
    $lines+=@('','## Как читать пробелы','',
        'Нет прямого источника — в документе и связанных входящих не задана явная ссылка на исходное сообщение. Карточка при этом может быть каноничной; связь нельзя достраивать по похожему имени или заголовку.',
        '',
        'Нет текущего положения или манеры речи — соответствующие сведения не выделены в карточке. Для исторического персонажа это может быть нормой. Ассистент проверяет исходный текст по задаче и не сочиняет отсутствующие сведения.',
        '',
        'Исторические разделы доступны отдельно от текущих. Старое положение и отношения нельзя автоматически объявлять действующими сегодня.',
        '',
        'Число размеченных фактов показывает объём специализированного реестра знаний. Остальная история продолжает храниться в сценах, карточках и источниках.')
    Write-WmmaText (Join-Path $root '10_Обслуживание/Покрытие_данных.md') (($lines -join [char]10)+[char]10)
    $summary|ConvertTo-Json
}
