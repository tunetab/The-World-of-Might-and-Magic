param([Parameter(Mandatory)][string]$Branch,[string[]]$SceneIds,[string[]]$FactIds,[string[]]$FocusIds,[string]$Situation,[switch]$SkipCheck)
$ErrorActionPreference='Stop';$provided=$PSBoundParameters
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $data=New-WmmaReadModel $root
    $path=Join-Path $root '09_Реестры/Контекст.json';$state=$data.state
    $entry=Resolve-WmmaBranch $data $Branch
    $knownScenes=@($data.entities_by_type['scene']|ForEach-Object {$_.id})
    $knownFacts=@($data.facts_by_id.Keys)
    if($provided.ContainsKey('SceneIds')){if(-not $SceneIds.Count){throw 'A branch needs at least one scene.'};foreach($id in $SceneIds){if($knownScenes -notcontains $id){throw "Unknown scene: $id"}};$entry.scene_ids=$SceneIds}
    if($provided.ContainsKey('FactIds')){foreach($id in $FactIds){if($knownFacts -notcontains $id){throw "Unknown fact: $id"}};$entry.fact_ids=$FactIds}
    if($provided.ContainsKey('FocusIds')){
        $validFocus=@($data.questions_by_id.Keys)+@($data.decisions_by_id.Keys)+@($data.fronts_by_id.Keys)
        foreach($id in $FocusIds){if($validFocus -notcontains $id){throw "Unknown focus ID: $id"}}
        $entry.focus_ids=$FocusIds
    }
    if($provided.ContainsKey('Situation')){if([string]::IsNullOrWhiteSpace($Situation)){throw 'Situation cannot be empty.'};$entry.situation=$Situation}
    Write-WmmaJson $path $state
    if(-not $SkipCheck){& (Join-Path $root 'tools/Завершить_ход.ps1')}
    "Updated branch context: $($entry.name)"
}
