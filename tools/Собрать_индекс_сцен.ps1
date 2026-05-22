param(
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$branchesRoot = Join-Path $root '01_Кампания\Ветки'
$archiveRoot = Join-Path $root '06_Архив_канона'
$targetPath = Join-Path $root '01_Кампания\00_Индекс_сцен.md'

function Get-RelativeProjectPath {
    param([string]$Path)

    return (($Path.Substring($root.Length).TrimStart('\', '/')) -replace '\\', '/')
}

function Get-Meta {
    param(
        [string]$Text,
        [string]$Field
    )

    if ($Text -match "(?m)^$([regex]::Escape($Field)):\s*(.*?)\s*$") {
        return $Matches[1].Trim()
    }

    return '-'
}

function Get-Title {
    param([string]$Text)

    if ($Text -match '(?m)^#\s+(.+?)\s*$') {
        return $Matches[1].Trim()
    }

    return 'Без названия'
}

$sceneRows = New-Object 'System.Collections.Generic.List[object]'

foreach ($file in Get-ChildItem -LiteralPath $branchesRoot -Recurse -File -Filter '*.md') {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    if ($text -notmatch '(?m)^type:\s*scene\s*$') {
        continue
    }

    $branch = Get-Meta -Text $text -Field 'branch'
    if ($branch -eq '-') {
        $branch = Split-Path -Leaf (Split-Path -Parent $file.FullName)
    }

    $sceneRows.Add([pscustomobject]@{
        Branch = $branch
        Chapter = Get-Meta -Text $text -Field 'chapter'
        Status = Get-Meta -Text $text -Field 'status'
        FrontId = Get-Meta -Text $text -Field 'front_id'
        Title = Get-Title -Text $text
        File = Get-RelativeProjectPath $file.FullName
    }) | Out-Null
}

$activeRows = @(
    $sceneRows |
        Where-Object { $_.Status -notin @('closed', 'archived') } |
        Sort-Object Branch, Chapter, File
)

$allRows = @($sceneRows | Sort-Object Branch, Chapter, File)
$archiveRows = @()
if (Test-Path -LiteralPath $archiveRoot) {
    $archiveRows = @(
        Get-ChildItem -LiteralPath $archiveRoot -Recurse -File -Filter '*.md' |
            ForEach-Object {
                $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName
                if ($text -match '(?m)^type:\s*archived_chapter\s*$') {
                    [pscustomobject]@{
                        Title = Get-Title -Text $text
                        Status = Get-Meta -Text $text -Field 'status'
                        File = Get-RelativeProjectPath $_.FullName
                    }
                }
            } |
            Sort-Object File
    )
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Индекс сцен') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: scene_index') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: support') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_индекс_сцен.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл генерируется автоматически. Для пересборки используй `.\tools\Собрать_индекс_сцен.ps1`.') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('## Активные сцены') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Ветка | Глава | Статус | FRONT-ID | Сцена | Файл |') | Out-Null
$lines.Add('| --- | --- | --- | --- | --- | --- |') | Out-Null

if ($activeRows.Count -eq 0) {
    $lines.Add('| - | - | - | - | Активных сцен не найдено | - |') | Out-Null
} else {
    foreach ($row in $activeRows) {
        $lines.Add("| $($row.Branch) | $($row.Chapter) | $($row.Status) | $($row.FrontId) | $($row.Title) | ``$($row.File)`` |") | Out-Null
    }
}

$lines.Add('') | Out-Null
$lines.Add('## Все сцены по веткам') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Ветка | Глава | Статус | FRONT-ID | Сцена | Файл |') | Out-Null
$lines.Add('| --- | --- | --- | --- | --- | --- |') | Out-Null

foreach ($row in $allRows) {
    $lines.Add("| $($row.Branch) | $($row.Chapter) | $($row.Status) | $($row.FrontId) | $($row.Title) | ``$($row.File)`` |") | Out-Null
}

$lines.Add('') | Out-Null
$lines.Add('## Архивные главы') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Глава | Статус | Файл |') | Out-Null
$lines.Add('| --- | --- | --- |') | Out-Null

foreach ($row in $archiveRows) {
    $lines.Add("| $($row.Title) | $($row.Status) | ``$($row.File)`` |") | Out-Null
}

Set-Content -LiteralPath $targetPath -Encoding UTF8 -Value $lines

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Built scene index: $(Get-RelativeProjectPath $targetPath)"
}
