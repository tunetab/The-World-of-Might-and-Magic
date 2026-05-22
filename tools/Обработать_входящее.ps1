param(
    [string]$Title = '',

    [string]$Summary = 'Обработано и перенесено в профильные файлы.',

    [string]$SourcePath = '',

    [string]$ScenePath = '',

    [string[]]$Links = @(),

    [ValidateSet('обработано', 'отложено', 'отклонено')]
    [string]$Status = 'обработано',

    [switch]$First,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
function Format-ProjectReference {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $reference = $Value.Trim() -replace '\\', '/'

    if ($reference -match '^`.+`$' -or $reference -match '^[a-z]+://') {
        return $reference
    }

    return "``$reference``"
}

function Convert-ToBulletText {
    param([string]$Value)

    $lines = @(
        $Value -split "\r?\n" |
            ForEach-Object { $_.Trim() } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )

    if ($lines.Count -eq 0) {
        return '- Обработано.'
    }

    return (($lines | ForEach-Object {
        if ($_ -match '^- ') {
            $_
        } else {
            "- $_"
        }
    }) -join "`r`n")
}

function Get-InboxEntries {
    param([string]$NewBody)

    return @([regex]::Matches($NewBody, '(?ms)^###\s+(.+?)\s*\r?\n(.*?)(?=^###\s+|\z)'))
}

function Select-InboxEntry {
    param(
        [object[]]$Entries,
        [string]$Needle,
        [switch]$UseFirst
    )

    if ($Entries.Count -eq 0) {
        throw 'No new inbox messages found.'
    }

    if ($UseFirst) {
        return $Entries[0]
    }

    if ([string]::IsNullOrWhiteSpace($Needle)) {
        throw 'Provide -Title or use -First.'
    }

    $exact = @(
        $Entries | Where-Object {
            $heading = $_.Groups[1].Value.Trim()
            $plainHeading = $heading -replace '^\d{4}-\d{2}-\d{2}\.\s*', ''
            $heading.Equals($Needle, [System.StringComparison]::OrdinalIgnoreCase) -or
                $plainHeading.Equals($Needle, [System.StringComparison]::OrdinalIgnoreCase)
        }
    )

    if ($exact.Count -eq 1) {
        return $exact[0]
    }

    $partial = @(
        $Entries | Where-Object {
            $heading = $_.Groups[1].Value.Trim()
            $heading.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        }
    )

    if ($partial.Count -eq 1) {
        return $partial[0]
    }

    if (($exact.Count + $partial.Count) -gt 1) {
        throw "Inbox title is ambiguous: $Needle"
    }

    throw "Inbox message not found: $Needle"
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$inbox = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
$codeFence = '```'

$section = [regex]::Match(
    $inbox,
    '(?ms)\A(.*?^## Новые сообщения\s*\r?\n)(.*?)(\r?\n## Обработанные входящие\s*\r?\n)(.*)\z'
)

if (-not $section.Success) {
    throw 'Inbox structure is broken: expected "## Новые сообщения" and "## Обработанные входящие".'
}

$newHeader = $section.Groups[1].Value.TrimEnd()
$newBody = $section.Groups[2].Value
$processedHeader = $section.Groups[3].Value.TrimEnd()
$processedRest = $section.Groups[4].Value.TrimStart()

$entries = Get-InboxEntries -NewBody $newBody
$selected = Select-InboxEntry -Entries $entries -Needle $Title -UseFirst:$First
$selectedHeading = $selected.Groups[1].Value.Trim()
$selectedBody = $selected.Groups[2].Value.Trim()

if (-not [string]::IsNullOrWhiteSpace($SourcePath)) {
    $sourceLine = Format-ProjectReference -Value $SourcePath
} elseif ($selectedBody -match '(?m)^Источник:\s*(.+?)\s*$') {
    $sourceLine = $Matches[1].Trim()
} else {
    $sourceLine = 'не указан'
}

$relatedLinks = New-Object 'System.Collections.Generic.List[string]'
$sceneReference = Format-ProjectReference -Value $ScenePath
if ($sceneReference) {
    $relatedLinks.Add($sceneReference) | Out-Null
}

foreach ($link in $Links) {
    $linkReference = Format-ProjectReference -Value $link
    if ($linkReference) {
        $relatedLinks.Add($linkReference) | Out-Null
    }
}

$processedLines = New-Object 'System.Collections.Generic.List[string]'
$processedLines.Add("### $selectedHeading") | Out-Null
$processedLines.Add('') | Out-Null
$processedLines.Add("Статус: $Status.") | Out-Null
$processedLines.Add("Источник: $sourceLine") | Out-Null

if ($relatedLinks.Count -gt 0) {
    $processedLines.Add("Связано: $($relatedLinks -join ', ')") | Out-Null
}

$processedLines.Add('') | Out-Null
$processedLines.Add('Кратко:') | Out-Null
$processedLines.Add('') | Out-Null
$processedLines.Add((Convert-ToBulletText -Value $Summary)) | Out-Null

$hasSourceFile = $sourceLine -match '`[^`]+\.md`'
$rawPattern = "(?ms)$([regex]::Escape($codeFence))text\s*\r?\n(.+?)\r?\n$([regex]::Escape($codeFence))"
if (-not $hasSourceFile -and $selectedBody -match $rawPattern) {
    $processedLines.Add('') | Out-Null
    $processedLines.Add('Исходное входящее:') | Out-Null
    $processedLines.Add('') | Out-Null
    $processedLines.Add("${codeFence}text") | Out-Null
    $processedLines.Add($Matches[1].Trim()) | Out-Null
    $processedLines.Add($codeFence) | Out-Null
}

$processedEntry = ($processedLines -join "`r`n").TrimEnd()
$newBodyWithoutEntry = ($newBody.Substring(0, $selected.Index) + $newBody.Substring($selected.Index + $selected.Length)).Trim()

if ([string]::IsNullOrWhiteSpace($newBodyWithoutEntry)) {
    $newBodyWithoutEntry = 'Пока нет новых необработанных сообщений.'
}

$updatedInbox = $newHeader + "`r`n`r`n" + $newBodyWithoutEntry + $processedHeader + "`r`n`r`n" + $processedEntry

if (-not [string]::IsNullOrWhiteSpace($processedRest)) {
    $updatedInbox += "`r`n`r`n" + $processedRest.TrimEnd()
}

$updatedInbox += "`r`n"

Set-Content -LiteralPath $inboxPath -Encoding UTF8 -Value $updatedInbox

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Processed inbox message: $selectedHeading"
}
