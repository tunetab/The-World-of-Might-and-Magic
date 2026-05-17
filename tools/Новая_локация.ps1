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

function Convert-ToProjectFileName {
    param([string]$Value)

    $safe = [regex]::Replace($Value.Trim(), '\s+', '_')
    $safe = $safe -replace '[\\/:*?"<>|]', ''
    $safe = $safe.Trim('_', '.', ' ')

    if ([string]::IsNullOrWhiteSpace($safe)) {
        throw 'Cannot build a safe file name from an empty location name.'
    }

    return $safe
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$fileName = Convert-ToProjectFileName -Value $Name
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

$locationIndexPath = Join-Path $root '04_Локации\00_Индекс_локаций.md'
$locationIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $locationIndexPath
$locationRow = "| $Name | active | $FrontId | ``$relativePath`` |"

if ($locationIndex -notmatch [regex]::Escape($relativePath)) {
    $locationIndex = $locationIndex.TrimEnd() + "`r`n$locationRow`r`n"
    Set-Content -LiteralPath $locationIndexPath -Encoding UTF8 -Value $locationIndex
}

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

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Created location: $relativePath"

