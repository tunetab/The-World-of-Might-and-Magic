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
$sourceRoot = Join-Path $root '08_Источники'
$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$targetPath = Join-Path $sourceRoot '00_Индекс_источников.md'

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

function Get-DateFromFileName {
    param([string]$FileName)

    if ($FileName -match '^(\d{4}-\d{2}-\d{2})_') {
        return $Matches[1]
    }

    return '-'
}

function Get-InboxState {
    param(
        [string]$InboxText,
        [string]$Reference
    )

    if ([string]::IsNullOrWhiteSpace($InboxText)) {
        return 'нет ссылки'
    }

    $escapedReference = [regex]::Escape($Reference)
    $newSection = ''
    $processedSection = ''

    $newMatch = [regex]::Match($InboxText, '(?ms)^## Новые сообщения\s*\r?\n(.+?)(?:\r?\n## Обработанные входящие|\z)')
    if ($newMatch.Success) {
        $newSection = $newMatch.Groups[1].Value
    }

    $processedMatch = [regex]::Match($InboxText, '(?ms)^## Обработанные входящие\s*\r?\n(.+)\z')
    if ($processedMatch.Success) {
        $processedSection = $processedMatch.Groups[1].Value
    }

    if ($newSection -match $escapedReference) {
        return 'новое'
    }

    if ($processedSection -match $escapedReference) {
        return 'обработано'
    }

    return 'нет ссылки'
}

$inboxText = ''
if (Test-Path -LiteralPath $inboxPath) {
    $inboxText = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
}

$rows = @(
    Get-ChildItem -LiteralPath $sourceRoot -File -Filter '*.md' |
        Where-Object { $_.Name -ne '00_Индекс_источников.md' } |
        ForEach-Object {
            $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName
            $relativePath = Get-RelativeProjectPath $_.FullName
            $date = Get-Meta -Text $text -Field 'date'

            if ($date -eq '-') {
                $date = Get-Meta -Text $text -Field 'received_real_date'
            }

            if ($date -eq '-') {
                $date = Get-DateFromFileName -FileName $_.Name
            }

            [pscustomobject]@{
                Date = $date
                Status = Get-Meta -Text $text -Field 'status'
                Type = Get-Meta -Text $text -Field 'type'
                Inbox = Get-InboxState -InboxText $inboxText -Reference $relativePath
                Title = Get-Title -Text $text
                File = $relativePath
            }
        } |
        Sort-Object Date, File
)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# Индекс источников') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('type: source_index') | Out-Null
$lines.Add('status: active') | Out-Null
$lines.Add('canon_level: support') | Out-Null
$lines.Add("generated_real_date: $today") | Out-Null
$lines.Add('generated_by: tools/Собрать_индекс_источников.ps1') | Out-Null
$lines.Add('---') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('Этот файл генерируется автоматически. Для пересборки используй `.\tools\Собрать_индекс_источников.ps1`.') | Out-Null
$lines.Add('') | Out-Null
$lines.Add('| Дата | Статус | Тип | Входящее | Источник | Файл |') | Out-Null
$lines.Add('| --- | --- | --- | --- | --- | --- |') | Out-Null

foreach ($row in $rows) {
    $lines.Add("| $($row.Date) | $($row.Status) | $($row.Type) | $($row.Inbox) | $($row.Title) | ``$($row.File)`` |") | Out-Null
}

Set-Content -LiteralPath $targetPath -Encoding UTF8 -Value $lines

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Built source index: $(Get-RelativeProjectPath $targetPath)"
}
