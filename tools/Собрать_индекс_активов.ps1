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
$assetRoot = Join-Path $root '05_Активы_персонажей'
$targetPath = Join-Path $assetRoot '00_Индекс_активов.md'

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

function Get-ExistingAssetRows {
    param([string]$IndexText)

    $rowsByFile = @{}
    $order = 0

    foreach ($line in ($IndexText -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -eq $cells -or $cells.Count -lt 5 -or $cells[0] -eq 'Владелец') {
            continue
        }

        if ($cells[4] -match '`([^`]+)`') {
            $order++
            $rowsByFile[$Matches[1]] = [pscustomobject]@{
                Order = $order
            }
        }
    }

    return $rowsByFile
}

$existingRows = Get-ExistingAssetRows -IndexText (Read-Text -Path $targetPath)
$assetRows = New-Object 'System.Collections.Generic.List[object]'

foreach ($file in Get-ChildItem -LiteralPath $assetRoot -Recurse -File -Filter '*.md') {
    if ($file.Name -eq '00_Индекс_активов.md' -or $file.Name -eq 'README.md') {
        continue
    }

    $text = Read-Text -Path $file.FullName
    if ((Get-Meta -Text $text -Field 'type') -ne 'character_asset') {
        continue
    }

    $relativePath = Get-RelativeProjectPath $file.FullName
    $owner = Get-Meta -Text $text -Field 'owner'
    $assetKind = Get-Meta -Text $text -Field 'asset_kind'
    $status = Get-Meta -Text $text -Field 'status'

    foreach ($required in @(
        [pscustomobject]@{ Name = 'owner'; Value = $owner },
        [pscustomobject]@{ Name = 'asset_kind'; Value = $assetKind },
        [pscustomobject]@{ Name = 'status'; Value = $status }
    )) {
        if ([string]::IsNullOrWhiteSpace($required.Value)) {
            throw "Character asset card is missing $($required.Name): $relativePath"
        }
    }

    $existing = $null
    if ($existingRows.ContainsKey($relativePath)) {
        $existing = $existingRows[$relativePath]
    }

    $order = 100000
    if ($existing) {
        $order = $existing.Order
    }

    $assetRows.Add([pscustomobject]@{
        Owner = $owner
        Asset = Get-Title -Text $text
        Kind = $assetKind
        Status = $status
        File = $relativePath
        Order = $order
    }) | Out-Null
}

$assetRows = @($assetRows | Sort-Object Order, Owner, Asset, File)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Индекс активов персонажей') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: character_assets_index') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: active') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_индекс_активов.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл генерируется автоматически из карточек активов персонажей. Подробные правила: `00_Инструкции_для_ИИ/05_Активы_персонажей.md`.') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Владелец | Актив | Тип | Статус | Файл |') | Out-Null
$lines.Add('| --- | --- | --- | --- | --- |') | Out-Null

foreach ($row in $assetRows) {
    $lines.Add('| ' + (@(
        Format-MarkdownCell $row.Owner
        Format-MarkdownCell $row.Asset
        Format-MarkdownCell $row.Kind
        Format-MarkdownCell $row.Status
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

"Built character asset index: $(Get-RelativeProjectPath $targetPath)"
}
