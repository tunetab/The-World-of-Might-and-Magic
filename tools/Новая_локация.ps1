param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Summary = 'Уточнить.',

    [string]$FrontId = '-',

    [switch]$Force,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$fileName = Convert-WmmaFileName -Value $Name
$relativePath = "04_Локации/$fileName.md"
$targetPath = Join-Path $root ($relativePath -replace '/', '\')

if ((Test-Path -LiteralPath $targetPath) -and -not $Force) {
    throw "Location file already exists: $relativePath"
}

$content = @"
# $Name

---
type: location
status: active
canon_level: active
front_id: $FrontId
---

## Кратко

$Summary

## География

Уточнить.

## Значение

Уточнить.

## Правитель / силы

Уточнить.

## Ресурсы

- Уточнить.

## История

- Уточнить.

## Текущие события

- Уточнить.

## Связанные персонажи

- Уточнить.
"@

Set-Content -LiteralPath $targetPath -Encoding UTF8 -Value $content

$mapPath = Join-Path $root '04_Локации\00_Карта_и_регионы.md'
$mapText = Get-Content -Raw -Encoding UTF8 -LiteralPath $mapPath
$row = "- ``$relativePath`` - $Summary"

if ($mapText -notmatch [regex]::Escape($relativePath)) {
    if ($mapText -notmatch '## Локации, добавленные инструментом') {
        $mapText = $mapText.TrimEnd() + @"

## Локации, добавленные инструментом

$row
"@
    } else {
        $mapText = $mapText.TrimEnd() + "`r`n$row"
    }

    Set-Content -LiteralPath $mapPath -Encoding UTF8 -Value $mapText
}

& (Join-Path $root 'tools\Собрать_индекс_локаций.ps1') -SkipCheck

if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}

"Created location: $relativePath"
}
