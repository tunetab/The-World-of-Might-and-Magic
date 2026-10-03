param(
    [Parameter(Mandatory)][string]$Text,
    [Parameter(Mandatory)][ValidateSet('confirmed','reported','rumor','dream','legend','inference','unknown')][string]$Truth,
    [Parameter(Mandatory)][string[]]$EvidenceIds,
    [string[]]$SubjectIds=@(),[string[]]$KnownTo=@(),
    [ValidateSet('public','restricted','gm')][string]$Visibility='restricted',
    [string]$StoryTime='см. событие-основание',[string]$ReportedBy='',
    [switch]$SkipCheck
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $data=New-WmmaReadModel $root;$byId=$data.entities_by_id
    foreach($id in @($EvidenceIds)+@($SubjectIds)+@($KnownTo)+@($ReportedBy)|Where-Object {$_}){if(-not $byId.ContainsKey($id)){throw "Unknown evidence/entity ID: $id"}}
    foreach($id in $KnownTo){if($byId[$id].type -ne 'character'){throw "Knowledge holder must be a character: $id"}}
    if([string]::IsNullOrWhiteSpace($Text) -or @($EvidenceIds|Where-Object {$_}).Count -eq 0){throw 'Knowledge text and at least one evidence ID are required.'}
    if($ReportedBy -and $byId[$ReportedBy].type -ne 'character'){throw 'ReportedBy must identify a character.'}
    $path=Join-Path $root '09_Реестры/Знания.json';$registry=$data.knowledge
    $max=0;foreach($fact in $registry.facts){if($fact.id -match '^FACT-(\d+)$'){$max=[Math]::Max($max,[int]$Matches[1])}}
    $id='FACT-{0:D3}' -f ($max+1)
    $proof=@($KnownTo|ForEach-Object {[pscustomobject]@{character_id=$_;evidence_ids=$EvidenceIds;basis='Круг знающих явно задан при записи факта.'}})
    $registry.facts=@($registry.facts)+@([pscustomobject]@{id=$id;text=$Text;truth=$Truth;evidence_ids=@($EvidenceIds|Sort-Object -Unique);subject_ids=@($SubjectIds|Sort-Object -Unique);known_to=@($KnownTo|Sort-Object -Unique);knowledge_evidence=$proof;visibility=$Visibility;story_time=$StoryTime;reported_by=$(if($ReportedBy){$ReportedBy}else{$null});recorded_on=(Get-Date -Format 'yyyy-MM-dd')})
    Write-WmmaJson $path $registry
    if(-not $SkipCheck){& (Join-Path $root 'tools/Завершить_ход.ps1')}
    "Created knowledge record: $id"
}
