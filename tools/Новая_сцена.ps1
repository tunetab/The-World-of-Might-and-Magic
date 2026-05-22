param(
    [Parameter(Mandatory = $true)]
    [string]$Branch,

    [Parameter(Mandatory = $true)]
    [string]$Title,

    [int]$Chapter = 2,

    [int]$Number = 0,

    [ValidateSet('draft', 'active', 'closed')]
    [string]$Status = 'draft',

    [string]$CanonLevel = 'draft',

    [string]$DateInStory = 'Уточнить.',

    [string]$Location = 'Уточнить.',

    [string]$FrontId = '-',

    [string]$Summary = 'Краткое описание сцены.',

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
        throw 'Cannot build a safe file name from an empty scene title.'
    }

    return $safe
}

function Get-RelativeProjectPath {
    param([string]$Path)

    return (($Path.Substring($root.Length).TrimStart('\', '/')) -replace '\\', '/')
}

function Get-DeclaredFrontIds {
    param([string]$FrontTrackerPath)

    $frontTracker = Get-Content -Raw -Encoding UTF8 -LiteralPath $FrontTrackerPath
    $frontIds = [regex]::Matches($frontTracker, '(?m)^\|\s*(FRONT-[A-Z0-9-]+)\s*\|') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    return @($frontIds)
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$frontTrackerPath = Join-Path $root '01_Кампания\06_Фронты_и_таймеры.md'
$declaredFrontIds = Get-DeclaredFrontIds -FrontTrackerPath $frontTrackerPath

if ($FrontId -ne '-' -and $declaredFrontIds -notcontains $FrontId) {
    throw "Unknown FRONT-ID: $FrontId. Add it to 01_Кампания/06_Фронты_и_таймеры.md first."
}

$branchRoot = Join-Path $root (Join-Path '01_Кампания\Ветки' $Branch)
if (-not (Test-Path -LiteralPath $branchRoot)) {
    New-Item -ItemType Directory -Path $branchRoot | Out-Null
}

if ($Number -le 0) {
    $maxNumber = 0
    foreach ($scene in Get-ChildItem -LiteralPath $branchRoot -File -Filter 'Сцена_*.md' -ErrorAction SilentlyContinue) {
        if ($scene.BaseName -match '^Сцена_(\d{3})_') {
            $sceneNumberValue = [int]$Matches[1]
            if ($sceneNumberValue -gt $maxNumber) {
                $maxNumber = $sceneNumberValue
            }
        }
    }

    $Number = $maxNumber + 1
}

if ($Number -lt 1 -or $Number -gt 999) {
    throw 'Scene number must be between 1 and 999.'
}

$sceneNumber = '{0:000}' -f $Number
$cleanTitle = [regex]::Replace($Title.Trim(), '^Сцена\s+\d{1,3}\.?\s*', '', 'IgnoreCase').Trim()
if ([string]::IsNullOrWhiteSpace($cleanTitle)) {
    throw 'Scene title cannot be empty.'
}

$fileTitle = Convert-ToProjectFileName -Value $cleanTitle
$fileName = "Сцена_${sceneNumber}_$fileTitle.md"
$targetPath = Join-Path $branchRoot $fileName
$relativePath = Get-RelativeProjectPath $targetPath

if ((Test-Path -LiteralPath $targetPath) -and -not $Force) {
    throw "Scene file already exists: $relativePath"
}

$content = @"
# Сцена $sceneNumber. $cleanTitle

---
type: scene
branch: $Branch
chapter: $Chapter
status: $Status
canon_level: $CanonLevel
date_in_story: $DateInStory
location: $Location
front_id: $FrontId
---

## Участники

- Уточнить.

## Событие

$Summary

## Что известно персонажу

- Уточнить.

## Решение

Что должен решить игрок.

## Варианты

1. Уточнить.
2. Уточнить.
3. Уточнить.

## Возможные последствия

- Уточнить.

## Что изменилось в каноне

- Новые факты: уточнить.
- Закрытые вопросы: уточнить.
- Новые вопросы: уточнить.
- Сдвинутые фронты / таймеры: уточнить.

## Файлы для обновления

- ``01_Кампания/02_Журнал_решений.md``, если принято решение.
- ``01_Кампания/03_Нерешенные_вопросы.md``, если появился новый открытый вопрос.
- ``01_Кампания/03_Закрытые_вопросы.md``, если вопрос закрыт.
- ``01_Кампания/06_Фронты_и_таймеры.md``, если изменился ``FRONT-*``.
- ``01_Кампания/00_Индекс_сцен.md``, пересобрать через ``.\tools\Собрать_индекс_сцен.ps1``.
- ``01_Кампания/07_Следующий_ход.md``, пересобрать через ``.\tools\Собрать_панель_хода.ps1``.

## Статус

Черновик / ожидает решения / решение принято / закрыто.
"@

Set-Content -LiteralPath $targetPath -Encoding UTF8 -Value $content

$global:LASTEXITCODE = 0
& (Join-Path $root 'tools\Собрать_индекс_сцен.ps1') -SkipCheck
if (-not $? -or $LASTEXITCODE -ne 0) {
    exit 1
}

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Created scene: $relativePath"
}
