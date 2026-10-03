param([switch]$AssignMissingIds,[switch]$SkipCheck)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $graph=New-WmmaGraph -Root $root -AssignMissingIds:$AssignMissingIds
    Write-WmmaJson (Join-Path $root '09_Реестры/Сущности.json') $graph
    "Entity graph: $($graph.entities.Count) entities, $($graph.edges.Count) relationships; $($graph.references.Count) document mentions kept separately."
    if(-not $SkipCheck){& (Join-Path $root 'tools/Проверить_контекст.ps1')}
}
