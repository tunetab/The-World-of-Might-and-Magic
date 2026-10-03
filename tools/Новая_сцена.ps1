param(
    [Parameter(Mandatory = $true)]
    [string]$Branch,

    [Parameter(Mandatory = $true)]
    [string]$Title,

    [int]$Chapter = 0,

    [int]$Number = 0,

    [ValidateSet('draft', 'active', 'closed')]
    [string]$Status = 'draft',

    [string]$CanonLevel = 'draft',

    [string]$DateInStory = 'Уточнить.',

    [string]$Location = 'Уточнить.',

    [string]$FrontId = '-',

    [string]$Summary = 'Краткое описание сцены.',

    [string]$RequestId = '',

    [string[]]$SourceIds = @(),

    [string[]]$ParticipantIds = @(),

    [string[]]$FrontIds = @(),

    [switch]$Force,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')

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
Assert-WmmaSingleLine $Branch 'Branch'
Assert-WmmaSingleLine $Title 'Title'
if($Branch -match '[\\/]' -or $Branch -in @('.','..')){throw 'Branch must be a single directory name.'}
foreach($value in @($DateInStory,$Location,$CanonLevel)){Assert-WmmaSingleLine $value 'Scene metadata'}
if($RequestId -and $RequestId -notmatch '^[A-Za-z0-9-]+$'){throw 'Invalid request ID.'}
if ($Chapter -le 0) { $Chapter = Get-WmmaCurrentChapter $root }
if($RequestId){
    $prior=@(Get-ChildItem -LiteralPath (Join-Path $root '01_Кампания/Ветки') -Recurse -Filter 'Сцена*.md' | Where-Object {(Get-WmmaMeta (Read-WmmaText $_.FullName) 'request_id') -eq $RequestId})
    if($prior.Count -gt 1){throw 'Duplicate scene request ID.'}
    if($prior.Count -eq 1){
        $old=Read-WmmaText $prior[0].FullName
        if((Get-WmmaMeta $old 'branch') -cne $Branch -or (Get-WmmaMeta $old 'chapter') -ne [string]$Chapter){throw 'Request ID belongs to another scene branch or chapter.'}
        'Created scene: '+(Get-WmmaRelativePath $root $prior[0].FullName);return
    }
}
$frontTrackerPath = Join-Path $root '01_Кампания\06_Фронты_и_таймеры.md'
$declaredFrontIds = Get-DeclaredFrontIds -FrontTrackerPath $frontTrackerPath

if ($FrontId -ne '-' -and $declaredFrontIds -notcontains $FrontId) {
    throw "Unknown FRONT-ID: $FrontId. Add it to 01_Кампания/06_Фронты_и_таймеры.md first."
}
foreach($id in $FrontIds){if($declaredFrontIds -notcontains $id){throw "Unknown front: $id"}}
$entityGraph=Read-WmmaJson (Join-Path $root '09_Реестры/Сущности.json')
$sources=@($entityGraph.entities|Where-Object {$_.type -like 'source*'}|ForEach-Object {$_.id})
# A source may have just been accepted before the next graph build.
$sources+=@(Get-ChildItem -LiteralPath (Join-Path $root '08_Источники') -Filter '*.md'|ForEach-Object {Get-WmmaMeta (Read-WmmaText $_.FullName) 'id'})
$characters=@($entityGraph.entities|Where-Object type -eq 'character'|ForEach-Object {$_.id})
foreach($id in $SourceIds){if(-not $id -or $sources -notcontains $id){throw "Unknown source ID: $id"}}
foreach($id in $ParticipantIds){if(-not $id -or $characters -notcontains $id){throw "Unknown character ID: $id"}}

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

$fileTitle = Convert-WmmaFileName -Value $cleanTitle
$fileName = "Сцена_${sceneNumber}_$fileTitle.md"
$targetPath = Join-Path $branchRoot $fileName
$relativePath = Get-WmmaRelativePath -Root $root $targetPath

if (Test-Path -LiteralPath $targetPath) {
    throw "Scene file already exists: $relativePath. Edit the existing document to preserve its ID and history; -Force cannot replace a scene."
}

$content = @"
# Сцена $sceneNumber. $cleanTitle

---
type: scene
id: SCENE-$([guid]::NewGuid().ToString('N'))
request_id: $RequestId
source_ids: $([string](ConvertTo-Json -InputObject @($SourceIds) -Compress))
participant_ids: $([string](ConvertTo-Json -InputObject @($ParticipantIds) -Compress))
front_ids: $([string](ConvertTo-Json -InputObject @($FrontIds) -Compress))
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

## Интересы участников

Уточнить по подтверждённым сценам и карточкам; не придумывать скрытые мотивы.

## Незавершённые обещания

Связанные решения и условия продолжения. Обещание не означает исполнение.

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

Write-WmmaText $targetPath $content

$global:LASTEXITCODE = 0
& (Join-Path $root 'tools\Собрать_индекс_сцен.ps1') -SkipCheck
if (-not $? -or $LASTEXITCODE -ne 0) {
    exit 1
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}

"Created scene: $relativePath"
}
