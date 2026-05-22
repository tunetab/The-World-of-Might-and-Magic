param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Role = 'уточнить',

    [ValidateSet('missing', 'planned', 'available')]
    [string]$PortraitStatus = 'missing',

    [string]$PortraitPath = 'null',

    [switch]$Force,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
function Convert-ToProjectFileName {
    param([string]$Value)

    $safe = [regex]::Replace($Value.Trim(), '\s+', '_')
    $safe = $safe -replace '[\\/:*?"<>|]', ''
    $safe = $safe.Trim('_', '.', ' ')

    if ([string]::IsNullOrWhiteSpace($safe)) {
        throw 'Cannot build a safe file name from an empty character name.'
    }

    return $safe
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$fileName = Convert-ToProjectFileName -Value $Name
$relativePath = "03_Персонажи/$fileName.md"
$targetPath = Join-Path $root ($relativePath -replace '/', '\')

if ((Test-Path -LiteralPath $targetPath) -and -not $Force) {
    throw "Character file already exists: $relativePath"
}

if ($PortraitStatus -eq 'available' -and ($PortraitPath -eq 'null' -or [string]::IsNullOrWhiteSpace($PortraitPath))) {
    throw 'portrait_status=available requires -PortraitPath.'
}

$content = @"
# $Name

---
type: character
status: active
canon_level: active
role: $Role
portrait: $PortraitPath
portrait_status: $PortraitStatus
---

## Кратко

$Role

## Текущее положение

Уточнить.

## Портрет

- Статус: $PortraitStatus
- Основной файл: $PortraitPath
- Идея для генерации: уточнить по карточке персонажа и стилю проекта.

## Прошлое

- Уточнить.

## Цели

- Уточнить.

## Связи

- Союзники: уточнить.
- Враги: уточнить.
- Семья: уточнить.
- Политические связи: уточнить.

## Секреты и неизвестное

- Уточнить.

## Связанные файлы

- ``03_Персонажи/00_Индекс_персонажей.md``
"@

Set-Content -LiteralPath $targetPath -Encoding UTF8 -Value $content

& (Join-Path $root 'tools\Собрать_индекс_персонажей.ps1') -SkipCheck

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Created character: $relativePath"
}
