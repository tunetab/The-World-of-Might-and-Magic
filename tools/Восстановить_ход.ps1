param([Parameter(Mandatory)][string]$Id,[ValidateSet('resume','restore')][string]$Mode='resume')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    if($Mode -eq 'restore'){Undo-WmmaTurn -Root $root -Id $Id | Select-Object id,status | ConvertTo-Json}
    else{Apply-WmmaTurn -Root $root -Id $Id | Select-Object id,status | ConvertTo-Json}
}
