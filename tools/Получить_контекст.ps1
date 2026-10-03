param([string]$Branch='', [string]$Query='', [string]$Entity='', [ValidateSet('gm','character')][string]$Audience='gm', [ValidateRange(500,20000)][int]$MaxWords=2000, [switch]$Json)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $result=Get-WmmaContext -Root $root -Branch $Branch -Query $Query -Entity $Entity -Audience $Audience -MaxWords $MaxWords
    if ($Json) { $result | ConvertTo-Json -Depth 30 } else { $result.text }
}
