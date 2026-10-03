param(
    [switch]$SkipPortraits,[switch]$SkipArchive,
    [switch]$SkipSceneIndex,[switch]$SkipSourceIndex,
    [switch]$SkipCharacterIndex,[switch]$SkipLocationIndex,[switch]$SkipAssetIndex
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new()
$OutputEncoding=[Text.UTF8Encoding]::new()
. (Join-Path $PSScriptRoot '_lib.ps1')
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
    function Invoke-BuildStep([string]$Script,[hashtable]$Arguments=@{}){
        "`n== $Script =="
        $global:LASTEXITCODE=0
        & (Join-Path $root "tools/$Script.ps1") @Arguments
        if(-not $? -or $LASTEXITCODE -ne 0){throw "Turn step failed: $Script"}
    }

    # Sources -> graph/indices/registry views -> context/memory -> audit -> checks.
    # Each builder runs once. Only disables legacy dependent rebuilds.
    Invoke-BuildStep 'Собрать_связи' @{AssignMissingIds=$true;SkipCheck=$true}
    foreach($step in @(
        @{script='Собрать_индекс_сцен';skip=$SkipSceneIndex},
        @{script='Собрать_индекс_источников';skip=$SkipSourceIndex},
        @{script='Собрать_индекс_персонажей';skip=$SkipCharacterIndex},
        @{script='Собрать_индекс_локаций';skip=$SkipLocationIndex},
        @{script='Собрать_индекс_активов';skip=$SkipAssetIndex}
    )){if(-not $step.skip){Invoke-BuildStep $step.script @{SkipCheck=$true}}}
    Invoke-BuildStep 'Собрать_решения' @{Only=$true}
    Invoke-BuildStep 'Собрать_вопросы' @{Only=$true}
    Invoke-BuildStep 'Собрать_срезы_реестров' @{SkipCheck=$true}
    Invoke-BuildStep 'Собрать_фронты' @{SkipCheck=$true}

    # No source writes below this point; all readers share this explicit snapshot.
    $data=New-WmmaReadModel $root
    Invoke-BuildStep 'Собрать_панель_хода' @{Data=$data;SkipCheck=$true}
    Invoke-BuildStep 'Собрать_контекст' @{Data=$data;SkipCheck=$true}
    Invoke-BuildStep 'Собрать_память' @{Data=$data}
    Invoke-BuildStep 'Собрать_аудит_данных' @{Data=$data}

    if(-not $SkipArchive){Invoke-BuildStep 'Проверить_архив'}
    if(-not $SkipPortraits){Invoke-BuildStep 'Проверить_портреты'}
    Invoke-BuildStep 'Проверить_проект'
    "`nTurn workspace is ready."
}
