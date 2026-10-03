param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$testRoot=Resolve-WmmaPath $root ('.wmma/audit-tests/'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
foreach($path in (Get-WmmaFileInventory $root).Keys){
    $target=Resolve-WmmaPath $testRoot $path
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
    [IO.File]::Copy((Resolve-WmmaPath $root $path),$target)
}
$passed=[Collections.Generic.List[string]]::new()
function Assert-True([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Expect-Failure([scriptblock]$Action,[string]$Message){
    $failure=$null
    try{& $Action|Out-Null}catch{$failure=$_}
    if(-not $failure){throw $Message}
    $global:LASTEXITCODE=0
    return $failure.Exception.Message
}
function Step([string]$Name,[scriptblock]$Action){
    $global:LASTEXITCODE=0
    & $Action
    if($LASTEXITCODE -ne 0){throw "Command failed: $Name"}
    $passed.Add($Name);Write-Host "PASS: $Name"
}
$nl=[char]10
$tick=[char]96
Step 'Empty metadata stays empty' {
    $doc=@('# Fixture','','---','type: scene','request_id:','source_ids: []','---') -join $nl
    Assert-True ((Get-WmmaMeta $doc 'request_id') -ceq '') 'Empty scalar consumed the following metadata line'
}
Step 'Images load beyond the Windows legacy path limit' {
    $original=Get-ChildItem -LiteralPath (Join-Path $root '11_Медиа') -Recurse -File -Filter '*.png'|Select-Object -First 1
    $folder=Join-Path $testRoot ('.wmma/long/'+('nested/'*30))
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    $path=Join-Path $folder 'image.png'
    [IO.File]::Copy($original.FullName,$path)
    $size=Get-WmmaImageSize $path
    Assert-True ($path.Length -gt 260 -and $size.Width -gt 0 -and $size.Height -gt 0) 'Long-path image failed'
}
Step 'Entity context keeps the requested asset within 500 words' {
    $context=Get-WmmaContext -Root $testRoot -Entity ASSET-0056 -MaxWords 500
    Assert-True ($context.text.Contains('флагман') -and $context.text.Contains('##')) 'Asset body missing'
    Assert-True (-not $context.text.Contains('Q-WORLD-044')) 'Unrelated global question displaced the asset'
    Assert-True ([regex]::Matches($context.text,'\S+').Count -le 500) 'Entity budget exceeded'
}
Step 'Historical character traits stay available with their evidence' {
    $memory=Read-WmmaJson (Join-Path $testRoot '09_Реестры/Память_персонажей.json')
    $profiles=@($memory.characters|Where-Object {$_.historical_sections.Count -gt 0})
    Assert-True ($profiles.Count -ge 27) 'Preserved character sections missing from memory'
    foreach($person in $profiles){foreach($section in $person.historical_sections){
        $history=Read-WmmaText (Join-Path $testRoot $section.path)
        Assert-True ($history.Contains($section.text)) 'Historical excerpt changed'
    }}
    $m=@($memory.characters|Where-Object character_id -eq CHAR-0136)[0]
    foreach($id in @('FACT-003','FACT-004','FACT-005')){Assert-True ($m.known_fact_ids -notcontains $id) "Unsupported audience knowledge: $id"}
}
Step 'Markdown and inline source links resolve once' {
    $source=@((Read-WmmaJson (Join-Path $testRoot '09_Реестры/Сущности.json')).entities|Where-Object type -eq source_note)[0]
    $text="$tick$($source.path)$tick [same](<$($source.path)>)"
    $refs=@(Get-WmmaDocumentReferences $testRoot '03_Персонажи/Fixture.md' $text)
    Assert-True ($refs.Count -eq 1 -and $refs[0] -ceq $source.path) 'Explicit source link resolution is incomplete or duplicated'
}
$raw=@('Исходный текст автора.','### Внутренний заголовок','## Обработанные входящие',("$tick"*3)+'json','status: авторский текст','Request-ID: MSG-fake',("$tick"*3),'Конец исходного текста.') -join $nl
Step 'Source intake preserves embedded headings fences and status lines' {
    & (Join-Path $testRoot 'tools/Принять_сообщение.ps1') -Title 'Вложенный Markdown' -Text $raw -RequestId MSG-audit-raw -SkipCheck|Out-Null
    $receipt=Get-WmmaReceipt $testRoot MSG-audit-raw
    Assert-True ([bool]$receipt.source_path) 'Default intake did not create source'
    & (Join-Path $testRoot 'tools/Обработать_входящее.ps1') -RequestId MSG-audit-raw -Status отложено -SkipCheck|Out-Null
    Assert-True ((Get-WmmaReceipt $testRoot MSG-audit-raw).state -eq 'deferred') 'Deferral discarded message'
    & (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') -RequestId MSG-audit-raw -Branch Аудит -Title 'Вложенный Markdown' -SkipCheck|Out-Null
    $receipt=Get-WmmaReceipt $testRoot MSG-audit-raw
    $source=Read-WmmaText (Join-Path $testRoot $receipt.source_path)
    Assert-True ((Get-WmmaRawMessage $source).Replace(([string][char]13),'') -ceq $raw) 'Author payload changed'
    Assert-True ((Get-WmmaMeta $source 'status') -eq 'processed') 'Source lifecycle did not advance'
    Assert-WmmaReceiptPayload $testRoot $receipt
    & (Join-Path $testRoot 'tools/Обработать_входящее.ps1') -RequestId MSG-audit-raw -SkipCheck|Out-Null
    [void](Expect-Failure {& (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') -RequestId MSG-audit-raw -Branch Другая -SkipCheck} 'Retry accepted a different branch')
    Write-WmmaText (Join-Path $testRoot $receipt.source_path) ($source.Replace('Конец исходного текста.','Повреждение.'))
    [void](Expect-Failure {& (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') -RequestId MSG-audit-raw -Branch Аудит -SkipCheck} 'Corrupted source passed retry')
    Write-WmmaText (Join-Path $testRoot $receipt.source_path) $source
}
Step 'Inbox-only payload and duplicate titles remain addressable by request ID' {
    foreach($id in @('MSG-audit-one','MSG-audit-two')){
        & (Join-Path $testRoot 'tools/Принять_сообщение.ps1') -Title 'Одинаковое название' -Text ($raw+$nl+$id) -RequestId $id -Mode inbox -SkipCheck|Out-Null
    }
    & (Join-Path $testRoot 'tools/Сцена_из_входящего.ps1') -RequestId MSG-audit-one -Branch Аудит -Title 'Одинаковое название' -SkipCheck|Out-Null
    & (Join-Path $testRoot 'tools/Обработать_входящее.ps1') -RequestId MSG-audit-two -SkipCheck|Out-Null
    foreach($id in @('MSG-audit-one','MSG-audit-two')){Assert-WmmaReceiptPayload $testRoot (Get-WmmaReceipt $testRoot $id)}
}
Step 'Scene replacement and invalid references fail before writing' {
    $r=Get-WmmaReceipt $testRoot MSG-audit-one
    $scenePath=Join-Path $testRoot $r.scene_path
    $original=Read-WmmaText $scenePath
    $number=[int]([regex]::Match((Split-Path -Leaf $scenePath),'Сцена_(\d+)').Groups[1].Value)
    [void](Expect-Failure {& (Join-Path $testRoot 'tools/Новая_сцена.ps1') -Branch Аудит -Title 'Одинаковое название' -Number $number -Force -SkipCheck} 'Scene force replaced stable identity')
    Assert-True ((Read-WmmaText $scenePath) -ceq $original) 'Rejected replacement wrote the scene'
    [void](Expect-Failure {& (Join-Path $testRoot 'tools/Новая_сцена.ps1') -Branch Аудит -Title 'Некорректная ссылка' -SourceIds SRC-missing -SkipCheck} 'Invalid source reference accepted')
}
Step 'Full rebuild validates embedded author messages and receipts' {
    Invoke-WmmaStageValidation $testRoot -Build
}
Step 'An original source can be requested as context' {
    $receipt=Get-WmmaReceipt $testRoot MSG-audit-raw
    $id=Get-WmmaMeta (Read-WmmaText (Join-Path $testRoot $receipt.source_path)) 'id'
    $context=Get-WmmaContext -Root $testRoot -Entity $id -MaxWords 500
    Assert-True ($context.text.Contains('Конец исходного текста.')) 'Original source returned an empty context packet'
}
Step 'Full graph validation detects tampering beyond content hashes' {
    $path=Join-Path $testRoot '09_Реестры/Сущности.json'
    $original=Read-WmmaText $path
    $graph=Read-WmmaJson $path
    $graph.edges=@($graph.edges|Select-Object -Skip 1)
    Write-WmmaJson $path $graph
    $result=& (Join-Path $testRoot 'tools/Проверить_контекст.ps1') -AsObject
    Assert-True (@($result.Errors|Where-Object {$_ -like '*graph*'}).Count -gt 0) 'Altered graph passed validation'
    Write-WmmaText $path $original
}
Step 'Validation detects stale memory and corrupted source payloads' {
    $path=Join-Path $testRoot '09_Реестры/Память_персонажей.json'
    $original=Read-WmmaText $path
    $memory=Read-WmmaJson $path
    $memory.characters[0].known_fact_ids=@('FACT-003')
    Write-WmmaJson $path $memory
    $result=& (Join-Path $testRoot 'tools/Проверить_контекст.ps1') -AsObject
    Assert-True (@($result.Errors|Where-Object {$_ -like '*memory is stale*'}).Count -eq 1) 'Stale character memory passed'
    Write-WmmaText $path $original
    $receipt=Get-WmmaReceipt $testRoot MSG-audit-raw
    $path=Join-Path $testRoot $receipt.source_path
    $original=Read-WmmaText $path
    Write-WmmaText $path ($original.Replace('Конец исходного текста.','Повреждение.'))
    $result=& (Join-Path $testRoot 'tools/Проверить_контекст.ps1') -AsObject
    Assert-True (@($result.Errors|Where-Object {$_ -like '*Invalid receipt MSG-audit-raw*'}).Count -eq 1) 'Corrupted original source passed validation'
    Write-WmmaText $path $original
}
# Transaction protocol tests use a tiny independent fixture. Real project build
# and staged validation are exercised by Проверить_систему_v2.ps1.
$tx=Join-Path $testRoot '.wmma/protocol'
[IO.Directory]::CreateDirectory($tx)|Out-Null
Write-WmmaText (Join-Path $tx 'tools/Проверить_проект.ps1') 'exit 0'
Write-WmmaText (Join-Path $tx 'tools/Завершить_ход.ps1') 'exit 0'
Write-WmmaText (Join-Path $tx 'existing.txt') 'before'
Step 'Interrupted snapshot resumes and refuses changed base files' {
    Start-WmmaTurn $tx TURN-copy 'Snapshot'|Out-Null
    $turn=Get-WmmaTurnPath $tx TURN-copy
    $mp=Join-Path $turn 'transaction.json'
    $m=Read-WmmaJson $mp;$m.status='copying';Write-WmmaJson $mp $m
    [IO.File]::Delete((Resolve-WmmaPath (Join-Path $turn 'stage') 'existing.txt'))
    Write-WmmaText (Join-Path $tx 'existing.txt') 'concurrent'
    [void](Expect-Failure {Start-WmmaTurn $tx TURN-copy 'Snapshot'} 'Changed source accepted during resume')
    Write-WmmaText (Join-Path $tx 'existing.txt') 'before'
    $m=Start-WmmaTurn $tx TURN-copy 'Snapshot'
    Assert-True ($m.status -eq 'editing' -and (Read-WmmaText (Join-Path $turn 'stage/existing.txt')) -ceq 'before') 'Partial snapshot was not resumed'
}
Step 'Stage failure reports the child diagnostic' {
    $turn=Get-WmmaTurnPath $tx TURN-copy
    $check=Join-Path $turn 'stage/tools/Проверить_проект.ps1'
    Write-WmmaText $check "Write-Output 'DIAGNOSTIC-audit-test'; exit 1"
    $message=Expect-Failure {Invoke-WmmaStageValidation (Join-Path $turn 'stage')} 'Failed stage accepted'
    Assert-True ($message.Contains('DIAGNOSTIC-audit-test')) 'Child failure detail swallowed'
    Write-WmmaText $check 'exit 0'
}
Step 'Plan tampering backups interrupted apply and repeat restore' {
    $turn=Get-WmmaTurnPath $tx TURN-copy
    Write-WmmaText (Join-Path $turn 'stage/existing.txt') 'after'
    Write-WmmaText (Join-Path $turn 'stage/new.txt') 'new'
    Prepare-WmmaTurn $tx TURN-copy|Out-Null
    $mp=Join-Path $turn 'transaction.json'
    $original=Read-WmmaText $mp;$m=Read-WmmaJson $mp
    $m.base.'existing.txt'=Get-WmmaHash 'tampered';Write-WmmaJson $mp $m
    [void](Expect-Failure {Apply-WmmaTurn $tx TURN-copy} 'Tampered base accepted')
    [void](Expect-Failure {Undo-WmmaTurn $tx TURN-copy} 'Tampered restore plan accepted')
    Write-WmmaText $mp $original
    [void](Expect-Failure {Apply-WmmaTurn $tx TURN-copy -FailAfter 1} 'Interruption did not fire')
    Assert-True ((Apply-WmmaTurn $tx TURN-copy).status -eq 'committed') 'Interrupted apply failed to resume'
    $backup=Join-Path $turn 'backup/existing.txt'
    Write-WmmaText $backup 'corrupt'
    [void](Expect-Failure {Undo-WmmaTurn $tx TURN-copy} 'Corrupt backup accepted')
    Assert-True ((Read-WmmaText (Join-Path $tx 'existing.txt')) -ceq 'after') 'Rejected restore modified target'
    Write-WmmaText $backup 'before'
    Assert-True ((Undo-WmmaTurn $tx TURN-copy).status -eq 'restored') 'Restore failed'
    Assert-True ((Undo-WmmaTurn $tx TURN-copy).status -eq 'restored') 'Repeated restore failed'
    Assert-True ((Read-WmmaText (Join-Path $tx 'existing.txt')) -ceq 'before' -and -not (Test-Path -LiteralPath (Join-Path $tx 'new.txt'))) 'Restore did not reproduce original files'
}
Step 'An empty prepared turn is valid and repeatable' {
    Start-WmmaTurn $tx TURN-empty 'Empty'|Out-Null
    Assert-True (@((Prepare-WmmaTurn $tx TURN-empty).changes).Count -eq 0) 'Empty turn gained changes'
    Assert-True ((Apply-WmmaTurn $tx TURN-empty).status -eq 'committed') 'Empty apply failed'
    Assert-True ((Undo-WmmaTurn $tx TURN-empty).status -eq 'restored') 'Empty restore failed'
}
[pscustomobject]@{Passed=$passed.Count;Tests=@($passed);Fixture=$testRoot}|ConvertTo-Json -Depth 6
