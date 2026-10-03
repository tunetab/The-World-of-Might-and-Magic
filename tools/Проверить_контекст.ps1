param([switch]$AsObject)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $errors=[Collections.Generic.List[string]]::new();$warnings=[Collections.Generic.List[string]]::new()
    try{$data=New-WmmaReadModel $root}catch{
        if(-not $AsObject){throw}
        return [pscustomobject]@{Errors=@($_.Exception.Message);Warnings=@();Entities=0;Facts=0}
    }
    $graph=$data.graph;$state=$data.state;$knowledge=$data.knowledge
    $decisions=$data.decisions;$questions=$data.questions
    $events=Read-WmmaJson (Join-Path $root '09_Реестры/Хронология_связей.json')
    $chapter=$data.chapter
    foreach($name in @('Сущности','Контекст','Знания','Хронология_связей')){
        try{
            $valid=Test-Json -Json (Read-WmmaText (Join-Path $root "09_Реестры/$name.json")) -SchemaFile (Join-Path $root '09_Реестры/Схемы/Память.schema.json') -ErrorAction Stop
            if(-not $valid){$errors.Add("Schema mismatch: $name")}
        }catch{$errors.Add("Schema mismatch: $name — $($_.Exception.Message)")}
    }
    $byId=@{};$uids=@{}
    try{
        $expectedGraph=New-WmmaGraph -Root $root
        if(($expectedGraph|ConvertTo-Json -Depth 50 -Compress) -cne ($graph|ConvertTo-Json -Depth 50 -Compress)){$errors.Add('Entity graph differs from documents, explicit sources or preserved history.')}
    }catch{$errors.Add('Cannot rebuild expected graph: '+$_.Exception.Message)}
    try{
        $memory=Read-WmmaJson (Join-Path $root '09_Реестры/Память_персонажей.json')
        if(($memory|ConvertTo-Json -Depth 50 -Compress) -cne ((New-WmmaCharacterMemory $root -Data $data)|ConvertTo-Json -Depth 50 -Compress)){$errors.Add('Character memory is stale or differs from its evidence.')}
    }catch{$errors.Add('Cannot validate character memory: '+$_.Exception.Message)}
    $receiptIds=[Collections.Generic.HashSet[string]]::new()
    foreach($receipt in (Read-WmmaJson (Join-Path $root '09_Реестры/Входящие.json')).receipts){
        if(-not $receiptIds.Add($receipt.request_id)){$errors.Add("Duplicate intake request ID: $($receipt.request_id)")}
        if($receipt.state -notin @('accepted','deferred','scene_created','processed','archived')){$errors.Add("Invalid intake state: $($receipt.request_id)")}
        try{Assert-WmmaReceiptPayload $root $receipt}catch{$errors.Add("Invalid receipt $($receipt.request_id): $($_.Exception.Message)")}
    }
    foreach($e in $graph.entities){
        if($byId.ContainsKey($e.id)){$errors.Add("Duplicate entity ID: $($e.id)")}
        $byId[$e.id]=$e
        $path=Resolve-WmmaPath $root $e.path
        if(-not (Test-Path -LiteralPath $path)){$errors.Add("Missing entity file: $($e.path)");continue}
        $text=Read-WmmaText $path
        if((Get-WmmaMeta $text 'id') -ne $e.id){$errors.Add("Entity ID mismatch: $($e.path)")}
        if((Get-WmmaHash $text) -cne $e.content_sha256){$errors.Add("Entity graph is stale: $($e.path)")}
    }
    foreach($doc in @(Get-WmmaDocuments $root)){
        if(-not $doc.id -or -not $byId.ContainsKey($doc.id)){$errors.Add("Document missing from graph: $($doc.path)")}
    }
    $edgeKeys=[Collections.Generic.HashSet[string]]::new()
    foreach($edge in @($graph.edges)+@($graph.references)){
        if(-not $edgeKeys.Add("$($edge.from)|$($edge.to)|$($edge.kind)|$($edge.evidence)|$($edge.scope)")){$errors.Add('Duplicate graph relationship.')}
        if($edge.from -eq $edge.to){$errors.Add("Self-referencing graph edge: $($edge.from)")}
        if(-not $byId.ContainsKey($edge.from) -or -not $byId.ContainsKey($edge.to)){$errors.Add("Dangling graph edge: $($edge.from) -> $($edge.to)")}
        if($edge.kind -in @('sourced_from','source_reference') -and $byId.ContainsKey($edge.to) -and $byId[$edge.to].type -notlike 'source*'){$errors.Add("Non-source provenance target: $($edge.to)")}
        if($edge.kind -eq 'participant' -and $byId.ContainsKey($edge.to) -and $byId[$edge.to].type -ne 'character'){$errors.Add("Participant is not a character: $($edge.to)")}
    }
    foreach($d in $decisions.decisions){
        if(-not $d.uid){$errors.Add("Missing decision UID: $($d.id)")}elseif($uids.ContainsKey($d.uid)){$errors.Add("Duplicate decision UID: $($d.uid)")}else{$uids[$d.uid]=$true}
        if($d.resolved_from -and $d.state -ne 'accepted'){$errors.Add("Invalid decision transition: $($d.id)")}
        if($d.resolved_from -and @($d.transitions|Where-Object {$_.from -eq $d.resolved_from -and $_.to -eq $d.id}).Count -eq 0){$errors.Add("Missing decision transition record: $($d.id)")}
    }
    $chapterFile=Resolve-WmmaPath $root $state.chapter_file
    if((Get-WmmaMeta (Read-WmmaText $chapterFile) 'status') -ne 'active'){$errors.Add('Context points to an inactive chapter.')}
    $factIds=@{}
    foreach($fact in $knowledge.facts){
        if($factIds.ContainsKey($fact.id)){$errors.Add("Duplicate fact ID: $($fact.id)")};$factIds[$fact.id]=$true
        if($fact.truth -notin @('confirmed','reported','rumor','dream','legend','inference','unknown')){$errors.Add("Invalid truth status: $($fact.id)")}
        if($fact.visibility -notin @('public','restricted','gm')){$errors.Add("Invalid knowledge visibility: $($fact.id)")}
        if(@($fact.evidence_ids).Count -eq 0){$errors.Add("Fact has no evidence: $($fact.id)")}
        foreach($id in @($fact.evidence_ids)+@($fact.subject_ids)+@($fact.known_to)){
            if(-not $byId.ContainsKey($id)){$errors.Add("Unknown entity in fact $($fact.id): $id")}
        }
        foreach($id in $fact.known_to){if($byId.ContainsKey($id) -and $byId[$id].type -ne 'character'){$errors.Add("Knowledge holder is not a character: $id")}}
        if($fact.reported_by -and (-not $byId.ContainsKey($fact.reported_by) -or $byId[$fact.reported_by].type -ne 'character')){$errors.Add("Unknown reporting character: $($fact.id)")}
        foreach($id in $fact.known_to){
            $proof=@($fact.knowledge_evidence|Where-Object {$_.character_id -eq $id})
            if(-not $proof.Count){$errors.Add("Knowledge holder has no evidence annotation: $($fact.id) -> $id")}
            foreach($record in $proof){if(-not @($record.evidence_ids).Count -or -not $record.basis){$errors.Add("Empty knowledge evidence annotation: $($fact.id) -> $id")}}
            foreach($record in $proof){foreach($evidenceId in $record.evidence_ids){if(-not $byId.ContainsKey($evidenceId)){$errors.Add("Unknown knowledge evidence: $evidenceId")}}}
        }
    }
    $frontIds=@($data.fronts_by_id.Keys)
    $focusIds=@($questions.questions|ForEach-Object {$_.id})+@($decisions.decisions|ForEach-Object {$_.id;$_.resolved_from}|Where-Object {$_})+$frontIds
    foreach($id in $state.fact_ids){if(-not $factIds.ContainsKey($id)){$errors.Add("Unknown global fact: $id")}}
    foreach($id in $state.scene_ids){if(-not $byId.ContainsKey($id) -or $byId[$id].type -ne 'scene'){$errors.Add("Unknown global scene: $id")}}
    foreach($id in $state.focus_ids){if($focusIds -notcontains $id){$errors.Add("Unknown global focus: $id")}}
    foreach($e in $graph.entities){foreach($id in $e.front_ids){if($frontIds -notcontains $id){$errors.Add("Unknown entity front: $($e.id) -> $id")}}}
    foreach($branch in $state.branches){
        if(-not $byId.ContainsKey($branch.character_id) -or $byId[$branch.character_id].type -ne 'character'){$errors.Add("Unknown branch character: $($branch.name)")}
        foreach($id in $branch.focus_ids){if($focusIds -notcontains $id){$errors.Add("Unknown branch focus: $id")}}
        foreach($id in $branch.scene_ids){if(-not $byId.ContainsKey($id) -or $byId[$id].type -ne 'scene'){$errors.Add("Unknown branch scene: $id")}}
        foreach($id in $branch.fact_ids){if(-not $factIds.ContainsKey($id)){$errors.Add("Unknown branch fact: $id")}}
        $profile=Read-WmmaText (Join-Path $root "01_Кампания/Ветки/$($branch.name)/00_Профиль_ветки.md")
        if((Get-WmmaMeta $profile 'current_chapter') -ne [string]$chapter){$errors.Add("Stale chapter in branch: $($branch.name)")}
        foreach($id in $branch.scene_ids){if($byId.ContainsKey($id) -and -not $profile.Contains($byId[$id].path)){$errors.Add("Branch omits linked shared scene: $($branch.name) -> $id")}}
        $expected=Get-WmmaContext -Root $root -Branch $branch.name -MaxWords 1800 -Data $data
        $actual=Read-WmmaText (Join-Path $root "01_Кампания/Контекст/$($branch.name).md")
        if($actual -cne $expected.text){$errors.Add("Stale branch packet: $($branch.name)")}
        if([regex]::Matches($actual,'\S+').Count -gt 1800){$errors.Add("Branch packet exceeds word budget: $($branch.name)")}
    }
    # An after relation must be acyclic. Overlaps is not an ordering edge.
    foreach($event in $events.relations){
        if(-not $byId.ContainsKey($event.from) -or -not $byId.ContainsKey($event.to)){$errors.Add('Unknown event endpoint.')}
        if($event.relation -notin @('after','overlaps','learned_after')){$errors.Add('Unknown temporal relation.')}
        foreach($id in $event.evidence_ids){if(-not $byId.ContainsKey($id)){$errors.Add("Unknown chronology evidence: $id")}}
        if($event.from -eq $event.to){$errors.Add('An event cannot reference itself in chronology.')}
    }
    foreach($cycle in @(Get-WmmaOrderCycles @($events.relations))){$errors.Add("Cyclic event order: $cycle")}
    $manifestPath=Join-Path $root '10_Обслуживание/Миграция_v2.json'
    if(Test-Path -LiteralPath $manifestPath){
        $manifest=Read-WmmaJson $manifestPath
        foreach($file in $manifest.protected_files){if((Get-FileHash -LiteralPath (Join-Path $root $file.path)).Hash.ToLowerInvariant() -cne $file.sha256){$errors.Add("Closed canon changed: $($file.path). Use an explicit retcon migration.")}}
        foreach($item in $manifest.history){
            $history=Read-WmmaText (Join-Path $root $item.history_path)
            $body=($history -split '<!-- ORIGINAL DOCUMENT -->\r?\n',2)[1].Replace("`r`n","`n").TrimEnd()
            if((Get-WmmaHash $body) -cne $item.text_sha256){$errors.Add("Historical preservation check failed: $($item.history_path)")}
        }
    }
    $selection=Get-WmmaSelection $root -Data $data
    $panel=Read-WmmaText (Join-Path $root '01_Кампания/07_Следующий_ход.md')
    foreach($q in @($selection.questions|Select-Object -First 8)){if(-not $panel.Contains($q.id)){$errors.Add("Panel omits selected question: $($q.id)")}}
    foreach($f in @($selection.fronts|Select-Object -First 10)){if(-not $panel.Contains($f.id)){$errors.Add("Panel omits selected front: $($f.id)")}}
    $result=[pscustomobject]@{Errors=@($errors);Warnings=@($warnings);Entities=$graph.entities.Count;Facts=$knowledge.facts.Count}
    if($AsObject){return $result}
    $result|Format-List
    if($errors.Count){throw 'Context checks failed.'}
    'Context checks completed successfully.'
}
