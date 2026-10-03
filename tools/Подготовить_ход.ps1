param([Parameter(Mandatory)][string]$Id)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    $result=Prepare-WmmaTurn -Root $root -Id $Id
    $result | Select-Object id,status,changes | ConvertTo-Json -Depth 10
}
