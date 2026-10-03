param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$testRoot=Resolve-WmmaPath $root ('.wmma/tests/'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$inventory=Get-WmmaFileInventory $root
foreach($path in $inventory.Keys){
    $target=Resolve-WmmaPath $testRoot $path
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
    [IO.File]::Copy((Resolve-WmmaPath $root $path),$target)
}
$passed=[Collections.Generic.List[string]]::new()
function Assert-True([bool]$Value,[string]$Message){if(-not $Value){throw $Message}}
function Expect-Failure([scriptblock]$Action,[string]$Message){
    $failed=$false
    try{& $Action|Out-Null}catch{$failed=$true}
    Assert-True $failed $Message
    $global:LASTEXITCODE=0
}
function Step([string]$Name,[scriptblock]$Action){
    $global:LASTEXITCODE=0
    & $Action
    if($LASTEXITCODE -ne 0){throw "Native/script failure in $Name"}
    $passed.Add($Name);Write-Host "PASS: $Name"
}
Step 'World questions and current scene outrank file order' {
    $s=Get-WmmaSelection $testRoot
    $ids=@($s.questions|Select-Object -First 8|ForEach-Object {$_.id})
    foreach($id in @('Q-WORLD-044','Q-WORLD-046','Q-C4-001')){Assert-True ($ids -contains $id) "Missing current question: $id"}
    Assert-True ($s.fronts[0].id -eq 'FRONT-THREE-MOONS') 'Main front omitted'
    $qPath=Join-Path $testRoot '09_Реестры/Вопросы.json';$q=Read-WmmaJson $qPath
    [array]::Reverse($q.questions);Write-WmmaJson $qPath $q
    $after=@((Get-WmmaSelection $testRoot).questions|Select-Object -First 8|ForEach-Object {$_.id})
    Assert-True (($ids -join ',') -ceq ($after -join ',')) 'Selection depends on storage order'
}
Step 'Character perspective excludes secrets and GM questions' {
    $a=Get-WmmaContext -Root $testRoot -Branch 'Александрос' -Audience character
    $m=Get-WmmaContext -Root $testRoot -Branch 'Михаэль' -Audience character
    Assert-True (-not $a.text.Contains('предполагаемой беременности')) 'Pregnancy leaked to Alexandros'
    Assert-True $m.text.Contains('предполагаемой беременности') 'Michael lost his own knowledge'
    Assert-True (-not $a.text.Contains('DEC-PENDING')) 'GM decisions leaked to character'
    Assert-True (-not $a.text.Contains('женщина в алых')) 'Michael dream leaked'
    $json=$a|ConvertTo-Json -Depth 30
    Assert-True (-not $json.Contains('DEC-PENDING') -and -not $json.Contains('предполагаемой беременности')) 'JSON metadata leaked GM knowledge'
}
Step 'Branch includes shared events and respects budget' {
    $n=Get-WmmaContext -Root $testRoot -Branch 'Никитосиос' -MaxWords 1000
    Assert-True (@($n.scene_ids).Count -ge 2) 'Shared scene not selected'
    Assert-True ([regex]::Matches($n.text,'\S+').Count -le 1000) 'Word limit exceeded'
    $n=Get-WmmaContext -Root $testRoot -Branch 'Никитосиос' -Query 'перемирие'
    Assert-True $n.text.Contains('Q-WORLD-046') 'Branch mission lost'
}
Step 'Closing decision preserves prose and stable identity' {
    $hot=Join-Path $testRoot '01_Кампания/00_Текущий_контекст.md'
    $text=Read-WmmaText $hot
    $hotOriginal=$text
    $decisionRegistryPath=Join-Path $testRoot '09_Реестры/Решения.json'
    $decisionRegistryOriginal=Read-WmmaText $decisionRegistryPath
    $text += @('','## Проверка абзаца','','Сохрани помилование и доклад Аругала. DEC-PENDING-137 остаётся в этом абзаце.','') -join [char]10
    Write-WmmaText $hot $text
    $before=@((Read-WmmaJson (Join-Path $testRoot '09_Реестры/Решения.json')).decisions|Where-Object {$_.id -eq 'DEC-PENDING-137'})[0]
    & (Join-Path $testRoot 'tools/Закрыть_решение.ps1') -PendingId DEC-PENDING-137 -AcceptedId DEC-900 -Choice 'Тестовый выбор' -Effect 'Только временная копия' -SkipCheck|Out-Null
    $after=Read-WmmaText $hot
    Assert-True $after.Contains('Сохрани помилование и доклад Аругала.') 'Unrelated prose was deleted'
    $d=@((Read-WmmaJson (Join-Path $testRoot '09_Реестры/Решения.json')).decisions|Where-Object {$_.id -eq 'DEC-900'})[0]
    Assert-True ($d.uid -eq $before.uid -and $d.resolved_from -eq 'DEC-PENDING-137') 'Decision identity was lost'
    & (Join-Path $testRoot 'tools/Закрыть_решение.ps1') -PendingId DEC-PENDING-137 -AcceptedId DEC-900 -Choice 'Тестовый выбор' -Effect 'Только временная копия' -SkipCheck|Out-Null
    # This is a mutator regression, not a new canonical player choice. Restore
    # fixture inputs before the independent end-to-end intake scenario below.
    Write-WmmaText $decisionRegistryPath $decisionRegistryOriginal
    Write-WmmaText $hot $hotOriginal
}
Step 'Repeated incoming creates exactly one scene in the current chapter' {
    $sceneOptions=@{Branch='Тест_v2';Title='Повторяемое сообщение';Text='Технический текст только для тестовой копии.';SkipCheck=$true}
    & (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') @sceneOptions|Out-Null
    & (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') @sceneOptions|Out-Null
    $scenes=@(Get-ChildItem -LiteralPath (Join-Path $testRoot '01_Кампания/Ветки/Тест_v2') -Filter 'Сцена*.md')
    Assert-True ($scenes.Count -eq 1) 'Repeated input created duplicate scenes'
    Assert-True ((Get-WmmaMeta (Read-WmmaText $scenes[0].FullName) 'chapter') -eq [string](Get-WmmaCurrentChapter $testRoot)) 'Wrong chapter default'
    Assert-True (@(Get-WmmaArrayMeta (Read-WmmaText $scenes[0].FullName) 'source_ids').Count -eq 1) 'Source association missing'
}
Step 'Intake recovers after source and inbox writes before receipt save' {
    $options=@{Title='Восстановление входящего';Text='Сообщение для проверки прерванной записи.';Mode='source';RequestId='MSG-recovery';SkipCheck=$true}
    & (Join-Path $testRoot 'tools/Принять_сообщение.ps1') @options|Out-Null
    $p=Join-Path $testRoot '09_Реестры/Входящие.json';$v=Read-WmmaJson $p
    $v.receipts=@($v.receipts|Where-Object {$_.request_id -ne 'MSG-recovery'});Write-WmmaJson $p $v
    & (Join-Path $testRoot 'tools/Принять_сообщение.ps1') @options|Out-Null
    $inbox=Read-WmmaText (Join-Path $testRoot '07_Черновики_и_идеи/Входящие_сообщения.md')
    Assert-True ([regex]::Matches($inbox,'Request-ID: MSG-recovery').Count -eq 1) 'Interrupted intake duplicated message'
    Assert-True ($null -ne (Get-WmmaReceipt $testRoot 'MSG-recovery')) 'Receipt was not recovered'
}
Step 'Full rebuild of modified fixture' {
    Invoke-WmmaStageValidation $testRoot -Build
}
Step 'Knowledge and branch tools reject unknown evidence without writes' {
    $path=Join-Path $testRoot '09_Реестры/Знания.json';$before=Read-WmmaText $path
    Expect-Failure {& (Join-Path $testRoot 'tools/Новое_знание.ps1') -Text 'Тест' -Truth reported -EvidenceIds SCENE-unknown -SkipCheck} 'Unknown knowledge evidence accepted'
    Assert-True ((Read-WmmaText $path) -ceq $before) 'Invalid knowledge changed registry'
    $path=Join-Path $testRoot '09_Реестры/Контекст.json';$before=Read-WmmaText $path
    Expect-Failure {& (Join-Path $testRoot 'tools/Обновить_контекст_ветки.ps1') -Branch 'Михаэль' -FocusIds Q-WORLD-999 -SkipCheck} 'Unknown focus accepted'
    Assert-True ((Read-WmmaText $path) -ceq $before) 'Invalid branch update changed registry'
}
Step 'Validation detects stale branch and cyclic chronology' {
    $profile=Join-Path $testRoot '01_Кампания/Ветки/Александрос/00_Профиль_ветки.md';$original=Read-WmmaText $profile
    Write-WmmaText $profile (Set-WmmaMeta $original 'current_chapter' '3')
    $r=& (Join-Path $testRoot 'tools/Проверить_контекст.ps1') -AsObject
    Assert-True (@($r.Errors|Where-Object {$_ -like '*Stale chapter*'}).Count -gt 0) 'Stale branch passed'
    Write-WmmaText $profile $original
    $p=Join-Path $testRoot '09_Реестры/Хронология_связей.json';$original=Read-WmmaText $p;$v=Read-WmmaJson $p
    $v.relations += [pscustomobject]@{from=$v.relations[0].to;to=$v.relations[0].from;relation='after';evidence_ids=$v.relations[0].evidence_ids;precision='relative'}
    Write-WmmaJson $p $v
    $r=& (Join-Path $testRoot 'tools/Проверить_контекст.ps1') -AsObject
    Assert-True (@($r.Errors|Where-Object {$_ -like '*Cyclic event*'}).Count -gt 0) 'Temporal cycle passed'
    Write-WmmaText $p $original
}
Step 'Prepared turn rejects edits made after preparation' {
    Start-WmmaTurn -Root $testRoot -Id TURN-test-recovery -Title 'Техническая транзакция'|Out-Null
    $turn=Get-WmmaTurnPath $testRoot TURN-test-recovery
    $stage=Join-Path $turn 'stage'
    $probe=Join-Path $stage '10_Обслуживание/Тест.txt'
    Write-WmmaText $probe 'Prepared content'
    Prepare-WmmaTurn -Root $testRoot -Id TURN-test-recovery|Out-Null
    Write-WmmaText $probe 'Changed after validation'
    Expect-Failure {Apply-WmmaTurn -Root $testRoot -Id TURN-test-recovery} 'Modified stage was applied'
    Write-WmmaText $probe 'Prepared content'
    Write-WmmaText (Join-Path $testRoot '10_Обслуживание/Тест.txt') 'Concurrent user content'
    Expect-Failure {Apply-WmmaTurn -Root $testRoot -Id TURN-test-recovery} 'Concurrent user edit was overwritten'
    [IO.File]::Delete((Resolve-WmmaPath $testRoot '10_Обслуживание/Тест.txt'))
}
Step 'Interrupted apply resumes once and restores its own changes' {
    Expect-Failure {Apply-WmmaTurn -Root $testRoot -Id TURN-test-recovery -FailAfter 1} 'Interruption was not simulated'
    $r=Apply-WmmaTurn -Root $testRoot -Id TURN-test-recovery
    Assert-True ($r.status -eq 'committed') 'Recovery did not finish'
    $again=Apply-WmmaTurn -Root $testRoot -Id TURN-test-recovery
    Assert-True ($again.status -eq 'committed') 'Repeat apply is not idempotent'
    $restored=Undo-WmmaTurn -Root $testRoot -Id TURN-test-recovery
    Assert-True ($restored.status -eq 'restored') 'Restore failed'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $testRoot '10_Обслуживание/Тест.txt'))) 'New test file was not removed on restoration'
}
Step 'Paths cannot escape project' {
    Expect-Failure {Resolve-WmmaPath $testRoot '../outside.txt'} 'Traversal accepted'
}
[pscustomobject]@{Passed=$passed.Count;Tests=@($passed);Fixture=$testRoot}|ConvertTo-Json -Depth 6
