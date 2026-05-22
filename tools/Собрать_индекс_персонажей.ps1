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
$characterRoot = Join-Path $root '03_Персонажи'
$targetPath = Join-Path $characterRoot '00_Индекс_персонажей.md'

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
        return 'Уточнить'
    }

    return (($Value.ToString() -replace '\|', '/') -replace '\r?\n', ' ').Trim()
}

function Convert-PortraitStatusToWord {
    param(
        [AllowNull()][string]$PortraitStatus,
        [AllowNull()][string]$Fallback
    )

    switch ($PortraitStatus) {
        'available' { return 'есть' }
        'planned' { return 'запланирован' }
        'missing' { return 'нужен' }
    }

    if (-not [string]::IsNullOrWhiteSpace($Fallback)) {
        return $Fallback
    }

    return 'нужен'
}

function Convert-MarkdownTableRow {
    param([string]$Line)

    if ($Line -notmatch '^\|.+\|$' -or $Line -match '^\|\s*-') {
        return $null
    }

    return ,($Line.Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
}

function Get-ExistingCharacterRows {
    param([string]$IndexText)

    $rowsByFile = @{}
    $groups = New-Object 'System.Collections.Generic.List[string]'
    $currentGroup = $null
    $order = 0

    foreach ($line in ($IndexText -split "\r?\n")) {
        if ($line -match '^##\s+(.+?)\s*$') {
            $heading = $Matches[1].Trim()
            if ($heading -ne 'Визуальные материалы') {
                $currentGroup = $heading
                if ($groups -notcontains $currentGroup) {
                    $groups.Add($currentGroup) | Out-Null
                }
            } else {
                $currentGroup = $null
            }
            continue
        }

        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -eq $cells -or $cells.Count -lt 4 -or $cells[0] -eq 'Персонаж') {
            continue
        }

        if ($cells[3] -match '`([^`]+)`') {
            $order++
            $rowsByFile[$Matches[1]] = [pscustomobject]@{
                Name = $cells[0]
                Role = $cells[1]
                Portrait = $cells[2]
                Group = $currentGroup
                Order = $order
            }
        }
    }

    return [pscustomobject]@{
        RowsByFile = $rowsByFile
        Groups = @($groups.ToArray())
    }
}

$existingIndex = Get-ExistingCharacterRows -IndexText (Read-Text -Path $targetPath)
$existingRows = $existingIndex.RowsByFile
$groupOrder = New-Object 'System.Collections.Generic.List[string]'
foreach ($group in $existingIndex.Groups) {
    if ($groupOrder -notcontains $group) {
        $groupOrder.Add($group) | Out-Null
    }
}

if ($groupOrder -notcontains 'Добавлено инструментом') {
    $groupOrder.Add('Добавлено инструментом') | Out-Null
}

$characterRows = New-Object 'System.Collections.Generic.List[object]'

foreach ($file in Get-ChildItem -LiteralPath $characterRoot -File -Filter '*.md') {
    if ($file.Name -like '00_*') {
        continue
    }

    $text = Read-Text -Path $file.FullName
    if ((Get-Meta -Text $text -Field 'type') -ne 'character') {
        continue
    }

    $relativePath = Get-RelativeProjectPath $file.FullName
    $existing = $null
    if ($existingRows.ContainsKey($relativePath)) {
        $existing = $existingRows[$relativePath]
    }

    $role = Get-Meta -Text $text -Field 'role'
    if (
        ([string]::IsNullOrWhiteSpace($role) -or $role -match '^(уточнить|Уточнить\.?)$') -and
        $existing -and
        -not [string]::IsNullOrWhiteSpace($existing.Role)
    ) {
        $role = $existing.Role
    }
    if ([string]::IsNullOrWhiteSpace($role)) {
        $role = 'Уточнить.'
    }

    $group = 'Добавлено инструментом'
    $order = 100000
    $portraitFallback = $null
    if ($existing) {
        if (-not [string]::IsNullOrWhiteSpace($existing.Group)) {
            $group = $existing.Group
        }
        $order = $existing.Order
        $portraitFallback = $existing.Portrait
    }

    if ($groupOrder -notcontains $group) {
        $groupOrder.Add($group) | Out-Null
    }

    $characterRows.Add([pscustomobject]@{
        Name = Get-Title -Text $text
        Role = $role
        Portrait = Convert-PortraitStatusToWord -PortraitStatus (Get-Meta -Text $text -Field 'portrait_status') -Fallback $portraitFallback
        File = $relativePath
        Group = $group
        Order = $order
    }) | Out-Null
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Индекс персонажей') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: character_index') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: active') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_индекс_персонажей.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл генерируется автоматически из карточек персонажей. Текущие разделы сохраняются как навигационные группы; новые карточки без прежней строки попадают в `Добавлено инструментом`.') | Out-Null

foreach ($group in $groupOrder) {
    $rows = @(
        $characterRows |
            Where-Object { $_.Group -eq $group } |
            Sort-Object Order, Name, File
    )

    if ($rows.Count -eq 0) {
        continue
    }

    $lines.Add('') | Out-Null
    $lines.Add("## $group") | Out-Null
    $lines.Add('') | Out-Null
    $lines.Add('| Персонаж | Роль | Портрет | Файл |') | Out-Null
    $lines.Add('| --- | --- | --- | --- |') | Out-Null

    foreach ($row in $rows) {
        $lines.Add('| ' + (@(
            Format-MarkdownCell $row.Name
            Format-MarkdownCell $row.Role
            Format-MarkdownCell $row.Portrait
            "``$($row.File)``"
        ) -join ' | ') + ' |') | Out-Null
    }
}

$lines.Add('') | Out-Null
$lines.Add('## Визуальные материалы') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Индекс портретов: `11_Медиа/Портреты_персонажей/Индекс_портретов.md`.') | Out-Null

Write-Utf8NoBom -Path $targetPath -Text (($lines -join "`n").TrimEnd() + "`n")

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Built character index: $(Get-RelativeProjectPath $targetPath)"
}
