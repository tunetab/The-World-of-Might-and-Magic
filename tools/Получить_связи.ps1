param(
    [Parameter(Mandatory)][Alias('Id')][string]$Entity,
    [ValidateRange(1,3)][int]$Depth=1,[ValidateRange(1,200)][int]$MaxNodes=20,
    [switch]$IncludeReferences,[switch]$IncludeHistory,[switch]$Json
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $result=Get-WmmaConnections -Root $root -Entity $Entity -Depth $Depth -MaxNodes $MaxNodes -IncludeReferences:$IncludeReferences -IncludeHistory:$IncludeHistory
    if($Json){$result|ConvertTo-Json -Depth 15;return}
    "Связи: $($result.entity.id) — $($result.entity.name)"
    "Глубина: $Depth; найдено узлов: $($result.connections.Count); предел: $MaxNodes."
    foreach($node in $result.connections){
        "- $($node.id) — $($node.name) [шаг $($node.distance), через $($node.via)]"
        "  Документ: $($node.path)"
        foreach($relation in $node.relations){
            "  $($relation.kind), $($relation.direction), $($relation.scope): $($relation.evidence)"
        }
    }
    if($result.truncated){'Достигнут предел узлов. Уточните ID или увеличьте MaxNodes; остальные связи сохранены.'}
    $result.note
}
