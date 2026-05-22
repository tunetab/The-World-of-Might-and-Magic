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
$locationRoot = Join-Path $root '04_Локации'
$targetPath = Join-Path $locationRoot '00_Индекс_локаций.md'

function Get-RelativeProjectPath {
    param([string]$Path)

    return (($Path.Substring($root.Length).TrimStart('\', '/')) -replace '\\', '/')
}

function Read-Text {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }

    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Text
    )

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-Meta {
    param(
        [string]$Text,
        [string]$Field
    )

    if ($Text -match "(?m)^$([regex]::Escape($Field)):\s*(.*?)\s*$") {
        return $Matches[1].Trim()
    }

    return $null
}

function Get-Title {
    param([string]$Text)

    if ($Text -match '(?m)^#\s+(.+?)\s*$') {
        return $Matches[1].Trim()
    }

    return 'Без названия'
}

function Format-MarkdownCell {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace($Value.ToString())) {
        return '-'
    }

    return (($Value.ToString() -replace '\|', '/') -replace '\r?\n', ' ').Trim()
}

function Convert-MarkdownTableRow {
    param([string]$Line)

    if ($Line -notmatch '^\|.+\|$' -or $Line -match '^\|\s*-') {
        return $null
    }

    return ,($Line.Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
}

function Get-ExistingLocationRows {
    param([string]$IndexText)

    $rowsByFile = @{}
    $order = 0

    foreach ($line in ($IndexText -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -eq $cells -or $cells.Count -lt 4 -or $cells[0] -eq 'Локация') {
            continue
        }

        if ($cells[3] -match '`([^`]+)`') {
            $order++
            $rowsByFile[$Matches[1]] = [pscustomobject]@{
                Order = $order
            }
        }
    }

    return $rowsByFile
}

$existingRows = Get-ExistingLocationRows -IndexText (Read-Text -Path $targetPath)
$locationRows = New-Object 'System.Collections.Generic.List[object]'

foreach ($file in Get-ChildItem -LiteralPath $locationRoot -File -Filter '*.md') {
    if ($file.Name -like '00_*' -or $file.Name -like '01_*' -or $file.Name -like '02_*') {
        continue
    }

    $text = Read-Text -Path $file.FullName
    if ((Get-Meta -Text $text -Field 'type') -ne 'location') {
        continue
    }

    $relativePath = Get-RelativeProjectPath $file.FullName
    $existing = $null
    if ($existingRows.ContainsKey($relativePath)) {
        $existing = $existingRows[$relativePath]
    }

    $status = Get-Meta -Text $text -Field 'status'

    $frontId = Get-Meta -Text $text -Field 'front_id'
    if ([string]::IsNullOrWhiteSpace($frontId)) {
        throw "Location card is missing front_id: $relativePath"
    }

    $order = 100000
    if ($existing) {
        $order = $existing.Order
    }

    $locationRows.Add([pscustomobject]@{
        Name = Get-Title -Text $text
        Status = $status
        FrontId = $frontId
        File = $relativePath
        Order = $order
    }) | Out-Null
}

$locationRows = @($locationRows | Sort-Object Order, Name, File)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Индекс локаций') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: location_index') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: active') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_индекс_локаций.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл генерируется автоматически из карточек локаций. Поле `front_id` хранится в front matter каждой карточки локации.') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('## Служебные карты и обзоры') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Файл | Назначение |') | Out-Null
$lines.Add('| --- | --- |') | Out-Null
$lines.Add('| `04_Локации/00_Карта_и_регионы.md` | словесная карта и крупные региональные заметки |') | Out-Null
$lines.Add('| `04_Локации/01_Регионы_Империи.md` | список регионов Империи |') | Out-Null
$lines.Add('| `04_Локации/02_Торговые_маршруты_и_пиратские_зоны.md` | торговые маршруты и пиратские зоны |') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('## Локации') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Локация | Статус | FRONT-ID | Файл |') | Out-Null
$lines.Add('| --- | --- | --- | --- |') | Out-Null

foreach ($row in $locationRows) {
    $lines.Add('| ' + (@(
        Format-MarkdownCell $row.Name
        Format-MarkdownCell $row.Status
        Format-MarkdownCell $row.FrontId
        "``$($row.File)``"
    ) -join ' | ') + ' |') | Out-Null
}

Write-Utf8NoBom -Path $targetPath -Text (($lines -join "`n").TrimEnd() + "`n")

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Built location index: $(Get-RelativeProjectPath $targetPath)"
}
