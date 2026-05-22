$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$archiveRoot = Join-Path $root '06_Архив_канона'
$errors = New-Object 'System.Collections.Generic.List[string]'
$warnings = New-Object 'System.Collections.Generic.List[string]'

function Add-Problem {
    param(
        [string]$Kind,
        [string]$Message
    )

    if ($Kind -eq 'Error') {
        $errors.Add($Message) | Out-Null
    } else {
        $warnings.Add($Message) | Out-Null
    }
}

function Get-RelativePath {
    param([string]$Path)

    if ($Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($root.Length).TrimStart('\', '/')
    }

    return $Path
}

if (-not (Test-Path -LiteralPath $archiveRoot)) {
    Add-Problem Error 'Archive folder is missing: 06_Архив_канона'
} else {
    $archiveFiles = Get-ChildItem -LiteralPath $archiveRoot -Recurse -File -Filter '*.md'

    foreach ($file in $archiveFiles) {
        $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        $relative = Get-RelativePath $file.FullName

        foreach ($forbiddenReference in @(
            '00_НАЧАТЬ_ОТСЮДА.md',
            '00_ТЕКУЩИЙ_КОНТЕКСТ_ДЛЯ_ИИ.md',
            '05_Текстовые_описания',
            '10_ПРОСТОЙ_РЕЖИМ_ДЛЯ_ТЕБЯ.md',
            '11_Медиа/Иллюстрации_сцен',
            '11_Медиа\Иллюстрации_сцен',
            '11_Медиа/Карты_и_схемы',
            '11_Медиа\Карты_и_схемы',
            'AI_ПРОМПТ_ДЛЯ_ВЕДЕНИЯ_ИГРЫ.md',
            'AI_ПРОМПТ_ДЛЯ_ФОТО.md'
        )) {
            if ($text.Contains($forbiddenReference)) {
                Add-Problem Error "Archive file contains legacy reference: ${relative} -> $forbiddenReference"
            }
        }

        if ($relative -like '*Завершенные_главы*') {
            if ($text -notmatch '(?m)^type:\s*archived_chapter\s*$') {
                Add-Problem Warning "Archived chapter has no type=archived_chapter: $relative"
            }

            if ($text -notmatch '(?m)^status:\s*closed\s*$') {
                Add-Problem Warning "Archived chapter should use status=closed: $relative"
            }

            if ($text -match '(?m)^status:\s*active\s*$') {
                Add-Problem Error "Archived chapter must not be active: $relative"
            }
        }
    }
}

[pscustomobject]@{
    ArchiveFiles = if (Test-Path -LiteralPath $archiveRoot) { (Get-ChildItem -LiteralPath $archiveRoot -Recurse -File -Filter '*.md').Count } else { 0 }
    Errors = $errors.Count
    Warnings = $warnings.Count
} | Format-List

if ($warnings.Count -gt 0) {
    "`nWarnings:"
    $warnings | Sort-Object | ForEach-Object { "- $_" }
}

if ($errors.Count -gt 0) {
    "`nErrors:"
    $errors | Sort-Object | ForEach-Object { "- $_" }
    exit 1
}

"`nArchive check completed successfully."
}
