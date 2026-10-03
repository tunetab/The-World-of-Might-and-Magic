param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$fixture=Resolve-WmmaPath $root ('.wmma/link-tests/'+[guid]::NewGuid().ToString('N'))
foreach($directory in @('01_Кампания/Главы','01_Кампания/Ветки','02_Лор','03_Персонажи','04_Локации','05_Активы_персонажей','08_Источники')){
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture $directory))
}
$passed=[Collections.Generic.List[string]]::new()
function Assert-True([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Expect-Failure([scriptblock]$Action,[string]$Message){
    $failed=$false
    try{& $Action|Out-Null}catch{$failed=$true}
    Assert-True $failed $Message
}
function Step([string]$Name,[scriptblock]$Action){& $Action;$passed.Add($Name);"PASS: $Name"}
function Write-FixtureDocument([string]$Path,[string]$Type,[string]$Id,[string]$Body,[string[]]$Meta=@()){
    $text=(@("# $Id",'','---',"type: $Type",'status: active',"id: $Id")+$Meta+@('---','',$Body,'')) -join "`n"
    Write-WmmaText (Join-Path $fixture $Path) $text
}
Write-FixtureDocument '01_Кампания/Главы/Глава.md' 'chapter' 'CHAPTER-test' 'Техническая фикстура.' @('chapter: 1')
Write-FixtureDocument '03_Персонажи/A.md' 'character' 'CHAR-a' @'
## Кратко
Тестовая карточка A.
## Связи
[B](B.md), [повтор](B.md), [сам](A.md), [упоминание](../04_Локации/Mention.md).
'@ @('aliases: ["Тёзка"]')
Write-FixtureDocument '03_Персонажи/B.md' 'character' 'CHAR-b' '## Кратко
Тестовая карточка B. [A](A.md).' @('aliases: ["Тёзка"]')
Write-FixtureDocument '04_Локации/Mention.md' 'location' 'LOC-mention' 'Только навигация.'
Write-FixtureDocument '03_Персонажи/C.md' 'character' 'CHAR-c' '## Кратко
Карточка без связанных сцен и фактов.'
Write-FixtureDocument '04_Локации/Past.md' 'location' 'LOC-past' 'Только историческое упоминание.'
Write-FixtureDocument '08_Источники/Source.md' 'source_note' 'SRC-test' 'Технический источник. [Сцена](../01_Кампания/Ветки/Scene.md).'
Write-FixtureDocument '01_Кампания/Ветки/Scene.md' 'scene' 'SCENE-test' @'
## Что известно персонажу
Тестовый фрагмент. [Источник](../../08_Источники/Source.md).
## Участники
[A](../../03_Персонажи/A.md), [B](../../03_Персонажи/B.md).
'@ @('chapter: 1','source_ids: ["SRC-test"]','participant_ids: ["CHAR-a", "CHAR-b"]','front_ids: ["FRONT-TEST"]')
Write-WmmaText (Join-Path $fixture '07_Черновики_и_идеи/Входящие_сообщения.md') '# Входящие'
Write-WmmaText (Join-Path $fixture '03_Персонажи/История/A.md') '# История

## Связи
[Старое место](../../04_Локации/Past.md).
'
Write-WmmaJson (Join-Path $fixture '10_Обслуживание/Миграция_v2.json') @{
    history=@(@{original_path='03_Персонажи/A.md';history_path='03_Персонажи/История/A.md'})
}
$graph=New-WmmaGraph $fixture
Write-WmmaJson (Join-Path $fixture '09_Реестры/Сущности.json') $graph
Write-WmmaJson (Join-Path $fixture '09_Реестры/Решения.json') @{
    decisions=@(@{id='DEC-007';state='accepted';choice='Технический выбор';resolved_from='DEC-PENDING-007';scene_ids=@('SCENE-test');link_paths=@('03_Персонажи/A.md')})
}
Write-WmmaJson (Join-Path $fixture '09_Реестры/Вопросы.json') @{
    questions=@(@{id='Q-WORLD-900';text='Технический вопрос';status='active';priority='высокий';owner='A'})
}
Write-WmmaJson (Join-Path $fixture '09_Реестры/Фронты.json') @{
    fronts=@(@{id='FRONT-TEST';name='Технический фронт'});urgent_forks=@();timers=@()
}
$state=@{
    branches=@(
        @{name='A';character_id='CHAR-a';scene_ids=@('SCENE-test');fact_ids=@('FACT-001','FACT-002','FACT-003');focus_ids=@('Q-WORLD-900','DEC-PENDING-007');situation='Техническая ветка A.'},
        @{name='B';character_id='CHAR-b';scene_ids=@('SCENE-test');fact_ids=@('FACT-001','FACT-002','FACT-003');focus_ids=@();situation='Техническая ветка B.'}
    );scene_ids=@('SCENE-test');fact_ids=@('FACT-001');focus_ids=@()
}
Write-WmmaJson (Join-Path $fixture '09_Реестры/Контекст.json') $state
Write-WmmaJson (Join-Path $fixture '09_Реестры/Знания.json') @{
    facts=@(
        @{id='FACT-001';text='PUBLIC_TOKEN';truth='confirmed';visibility='public';known_to=@();subject_ids=@('CHAR-a');evidence_ids=@('SRC-test')},
        @{id='FACT-002';text='PRIVATE_TOKEN';truth='reported';visibility='restricted';known_to=@('CHAR-a');subject_ids=@('CHAR-a');evidence_ids=@('SCENE-test')},
        @{id='FACT-003';text='GM_TOKEN';truth='unknown';visibility='gm';known_to=@('CHAR-a');subject_ids=@('CHAR-a');evidence_ids=@('SRC-test')}
    )
}
$data=New-WmmaReadModel $fixture
Step 'Graph partitions mentions without losing provenance' {
    Assert-True ($graph.schema_version -eq 3) 'Wrong graph version'
    Assert-True (@($graph.edges|Where-Object kind -eq references).Count -eq 0) 'Mention entered semantic edges'
    Assert-True (@($graph.references|Where-Object kind -ne references).Count -eq 0) 'Typed relationship became a mention'
    Assert-True (@($graph.edges|Where-Object {$_.from -eq 'SCENE-test' -and $_.to -eq 'SRC-test'}).Count -eq 2) 'Explicit and textual source evidence must both survive'
    Assert-True ($data.entities_by_id['SCENE-test'].source_ids -contains 'SRC-test') 'Source projection was lost'
    Assert-True (@($graph.references|Where-Object {$_.from -eq $_.to}).Count -eq 0) 'Self navigation retained'
    $refs=@(Get-WmmaDocumentReferences $fixture '03_Персонажи/A.md' '[B](B.md) [again](B.md) [escape](../../../outside.md)' -KnownPaths $data.entities_by_path)
    Assert-True ($refs.Count -eq 1 -and $refs[0] -eq '03_Персонажи/B.md') 'Indexed resolution duplicated or escaped a path'
}
Step 'Nearest connections join registries and return each node once' {
    $result=Get-WmmaConnections $fixture CHAR-a -MaxNodes 200 -Data $data
    $ids=@($result.connections.id)
    foreach($id in @('SCENE-test','FACT-002','Q-WORLD-900','DEC-007')){Assert-True ($ids -contains $id) "Missing explicit registry connection: $id"}
    Assert-True ($ids -notcontains 'LOC-mention' -and $ids -notcontains 'LOC-past') 'Default expanded document mentions'
    Assert-True (($ids|Sort-Object -Unique).Count -eq $ids.Count) 'Duplicate connected node'
    $source=Get-WmmaConnections $fixture SCENE-test -MaxNodes 200 -Data $data
    Assert-True (@($source.connections|Where-Object id -eq SRC-test).Count -eq 1) 'Multiple proofs produced duplicate source nodes'
    Assert-True (@(($source.connections|Where-Object id -eq SRC-test).relations).Count -eq 2) 'A direct source proof was discarded'
}
Step 'Mention and history expansion require their own switches' {
    $mentions=Get-WmmaConnections $fixture CHAR-a -IncludeReferences -MaxNodes 200 -Data $data
    Assert-True ($mentions.connections.id -contains 'LOC-mention') 'Requested mention missing'
    Assert-True ($mentions.connections.id -notcontains 'LOC-past') 'History expanded implicitly'
    $history=Get-WmmaConnections $fixture CHAR-a -IncludeReferences -IncludeHistory -MaxNodes 200 -Data $data
    Assert-True ($history.connections.id -contains 'LOC-past') 'Historical reference lost'
    Assert-True (@(($history.connections|Where-Object id -eq LOC-past).relations|Where-Object scope -eq historical).Count -gt 0) 'Historical scope lost'
}
Step 'Reciprocal cycles obey both depth and node limits' {
    $before=$data|ConvertTo-Json -Depth 40 -Compress
    $result=Get-WmmaConnections $fixture CHAR-a -Depth 3 -MaxNodes 200 -IncludeReferences -IncludeHistory -Data $data
    $ids=@($result.connections.id)
    Assert-True ($ids -notcontains 'CHAR-a' -and ($ids|Sort-Object -Unique).Count -eq $ids.Count) 'Traversal revisited a node'
    foreach($node in $result.connections){
        Assert-True ($node.distance -le 3 -and ($node.via -eq 'CHAR-a' -or $ids -contains $node.via)) 'Broken bounded route'
    }
    $limited=Get-WmmaConnections $fixture CHAR-a -Depth 3 -MaxNodes 1 -Data $data
    Assert-True ($limited.connections.Count -eq 1 -and $limited.truncated) 'Node cap was ignored or hidden'
    Assert-True (($data|ConvertTo-Json -Depth 40 -Compress) -ceq $before) 'Read query changed its snapshot'
}
Step 'Ambiguous names fail and decision aliases retain identity' {
    Expect-Failure {Resolve-WmmaEntity $data 'Тёзка'} 'Ambiguous alias was guessed'
    Assert-True ((Resolve-WmmaEntity $data 'CHAR-a').id -eq 'CHAR-a') 'Stable ID did not resolve'
    $resolved=Get-WmmaConnections $fixture DEC-PENDING-007 -Data $data
    Assert-True ($resolved.entity.id -eq 'DEC-007' -and $resolved.requested_id -eq 'DEC-PENDING-007') 'Decision transition lost'
    Expect-Failure {New-WmmaIdIndex @(@{id='CHAR-a'},@{id='CHAR-a'})} 'Duplicate identity accepted'
}
Step 'Context and memory share the same knowledge boundary' {
    $a=Get-WmmaContext $fixture -Branch A -Audience character -Data $data|ConvertTo-Json -Depth 20
    $b=Get-WmmaContext $fixture -Branch B -Audience character -Data $data|ConvertTo-Json -Depth 20
    Assert-True ($a.Contains('PRIVATE_TOKEN') -and $a.Contains('PUBLIC_TOKEN') -and -not $a.Contains('GM_TOKEN')) 'Viewer A knowledge boundary failed'
    Assert-True ($b.Contains('PUBLIC_TOKEN') -and -not $b.Contains('PRIVATE_TOKEN') -and -not $b.Contains('GM_TOKEN')) 'Viewer B knowledge boundary failed'
    $memory=New-WmmaCharacterMemory $fixture -Data $data
    $aMemory=$memory.characters|Where-Object character_id -eq CHAR-a
    Assert-True ($aMemory.known_fact_ids -contains 'FACT-002' -and $aMemory.known_fact_ids -notcontains 'FACT-003') 'Memory disagrees with character context'
    $emptyMemory=$memory.characters|Where-Object character_id -eq CHAR-c
    Assert-True ($emptyMemory.world_fact_ids.Count -eq 0 -and $emptyMemory.scene_reference_ids.Count -eq 0) 'Missing index groups introduced null references'
}
Step 'Read snapshots are explicit and refresh after source changes' {
    $path=Join-Path $fixture '09_Реестры/Вопросы.json'
    $questions=Read-WmmaJson $path;$questions.questions[0].text='REFRESHED_TOKEN';Write-WmmaJson $path $questions
    $fresh=New-WmmaReadModel $fixture
    Assert-True ($fresh.questions_by_id['Q-WORLD-900'].text -eq 'REFRESHED_TOKEN') 'New operation reused stale data'
    Assert-True ($data.questions_by_id['Q-WORLD-900'].text -ne 'REFRESHED_TOKEN') 'Existing snapshot changed underneath a reader'
    Expect-Failure {Resolve-WmmaReadModel $root $data} 'Snapshot crossed project roots'
}
Step 'Long converging chronology is checked without recursion' {
    $relations=[Collections.Generic.List[object]]::new()
    for($i=0;$i -lt 3000;$i++){
        $relations.Add(@{from="EVENT-$i";to=('EVENT-'+($i+1));relation='after'})
        if($i -gt 0){$relations.Add(@{from=('EVENT-'+($i-1));to=('EVENT-'+($i+1));relation='learned_after'})}
    }
    $relations.Add(@{from='EVENT-3000';to='EVENT-0';relation='overlaps'})
    Assert-True (@(Get-WmmaOrderCycles @($relations)).Count -eq 0) 'Converging paths or overlaps produced a cycle'
    $relations.Add(@{from='EVENT-3000';to='EVENT-2999';relation='learned_after'})
    $cycles=@(Get-WmmaOrderCycles @($relations))
    Assert-True ($cycles.Count -eq 1 -and $cycles[0].Contains('EVENT-2999') -and $cycles[0].Contains('EVENT-3000')) 'Ordering cycle was missed or duplicated'
}
Step 'Rebuild dates do not rewrite unchanged views or hide changed data' {
    $path=Join-Path $fixture '.wmma/generated.md'
    $old="# Index`n`ngenerated_real_date: 2026-09-21`n`nOriginal row.`n"
    Write-WmmaText $path $old
    $stamp=(Get-Item -LiteralPath $path).LastWriteTimeUtc
    Write-WmmaGeneratedText $path ($old.Replace('2026-09-21','2026-09-22'))
    Assert-True ((Read-WmmaText $path) -ceq $old -and (Get-Item -LiteralPath $path).LastWriteTimeUtc -eq $stamp) 'Date-only rebuild rewrote a view'
    $changed=$old.Replace('Original row.','New row.').Replace('2026-09-21','2026-09-22')
    Write-WmmaGeneratedText $path $changed
    Assert-True ((Read-WmmaText $path) -ceq $changed) 'Substantive view change was suppressed'
    $path=Join-Path $fixture '.wmma/generated.json'
    $json=[ordered]@{updated_real_date='2026-09-21';record=[ordered]@{updated_real_date='2026-09-21'}}|ConvertTo-Json
    Write-WmmaText $path $json
    $new=$json.Replace('  "updated_real_date": "2026-09-21"','  "updated_real_date": "2026-09-22"')
    # Change just the top-level generation date, then also the record's date.
    $top=[regex]::Replace($json,'(?m)^  "updated_real_date": "2026-09-21"','  "updated_real_date": "2026-09-22"')
    Write-WmmaGeneratedText $path $top -Format json
    Assert-True ((Read-WmmaText $path) -ceq $json) 'JSON generation date caused churn'
    Write-WmmaGeneratedText $path $new -Format json
    Assert-True ((Read-WmmaText $path) -ceq $new) 'Nested source date was ignored'
}
Step 'Shared parsers preserve empty metadata filenames and table cells' {
    $text="# Fixture`n`n---`nstatus:`nnext: value`n---`nstatus: author text"
    Assert-True ((Get-WmmaMeta $text status -Default '-') -ceq '') 'Empty metadata consumed the following field'
    Assert-True ((Get-WmmaMeta $text absent -Default '-') -ceq '-') 'Missing metadata default lost'
    Assert-True ((Format-WmmaTableCell $null -Empty '-') -ceq '-') 'Missing-cell policy changed'
    Assert-True ((Format-WmmaTableCell "a|b`nc") -ceq 'a/b c') 'Table cell formatting changed'
    Assert-True ((Convert-WmmaFileName 'Тест / A?' -Lowercase) -ceq 'тест__a') 'Source filename convention changed'
    Expect-Failure {Read-WmmaRegistry $fixture 'Missing'} 'Missing registry silently reconstructed'
    foreach($script in @('Собрать_решения','Собрать_вопросы','Собрать_фронты')){
        Expect-Failure {& (Join-Path $root "tools/$script.ps1") -ImportFromMarkdown -SkipCheck} 'Obsolete import flag was silently accepted'
    }
}
Step 'Graph schema rejects mixed layers and missing evidence' {
    $schema=Join-Path $root '09_Реестры/Схемы/Память.schema.json'
    Assert-True (Test-Json -Json ($graph|ConvertTo-Json -Depth 30) -SchemaFile $schema -ErrorAction Stop) 'Valid graph rejected'
    $invalid=$graph|ConvertTo-Json -Depth 30|ConvertFrom-Json
    $invalid.edges+=@($invalid.references[0])
    Expect-Failure {Test-Json -Json ($invalid|ConvertTo-Json -Depth 30) -SchemaFile $schema -ErrorAction Stop} 'Mention accepted as a semantic edge'
    $invalid=$graph|ConvertTo-Json -Depth 30|ConvertFrom-Json
    $invalid.edges[0].evidence=''
    Expect-Failure {Test-Json -Json ($invalid|ConvertTo-Json -Depth 30) -SchemaFile $schema -ErrorAction Stop} 'Unproven relationship accepted'
}
[pscustomobject]@{Passed=$passed.Count;Tests=@($passed);Fixture=$fixture}|ConvertTo-Json -Depth 5
