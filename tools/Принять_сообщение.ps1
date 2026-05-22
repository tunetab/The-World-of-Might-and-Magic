param(
    [Parameter(Mandatory = $true)]
    [string]$Title,

    [string]$Text = '',

    [string]$TextPath = '',

    [ValidateSet('inbox', 'source')]
    [string]$Mode = 'inbox',

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
function Convert-ToProjectFileName {
    param([string]$Value)

    $safe = $Value.Trim().ToLowerInvariant()
    $safe = [regex]::Replace($safe, '\s+', '_')
    $safe = $safe -replace '[\\/:*?"<>|]', ''
    $safe = $safe.Trim('_', '.', ' ')

    if ([string]::IsNullOrWhiteSpace($safe)) {
        throw 'Cannot build a safe file name from an empty message title.'
    }

    return $safe
}

function Get-RelativeProjectPath {
    param([string]$Path)

    return (($Path.Substring($root.Length).TrimStart('\', '/')) -replace '\\', '/')
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$codeFence = '```'

if (-not [string]::IsNullOrWhiteSpace($TextPath)) {
    $resolvedTextPath = (Resolve-Path -LiteralPath $TextPath).Path
    $Text = Get-Content -Raw -Encoding UTF8 -LiteralPath $resolvedTextPath
}

if ([string]::IsNullOrWhiteSpace($Text)) {
    throw 'Provide message text through -Text or -TextPath.'
}

$sourceReference = 'вручную через `tools/Принять_сообщение.ps1`'

if ($Mode -eq 'source') {
    $sourceRoot = Join-Path $root '08_Источники'
    $sourceFileName = "$today`_$(Convert-ToProjectFileName -Value $Title).md"
    $sourcePath = Join-Path $sourceRoot $sourceFileName

    if (Test-Path -LiteralPath $sourcePath) {
        throw "Source file already exists: $(Get-RelativeProjectPath $sourcePath)"
    }

    $sourceContent = @"
# $Title

---
type: source_note
status: new
canon_level: draft
received_real_date: $today
---

${codeFence}text
$Text
${codeFence}
"@

    Set-Content -LiteralPath $sourcePath -Encoding UTF8 -Value $sourceContent
    $sourceReference = "``$(Get-RelativeProjectPath $sourcePath)``"
}

$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$inbox = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
$entry = @"
### $today. $Title

Статус: новое.
Источник: $sourceReference

${codeFence}text
$Text
${codeFence}

"@

if ($inbox -notmatch '(?m)^## Новые сообщения\s*$') {
    throw 'Inbox section not found: ## Новые сообщения'
}

$inbox = $inbox -replace '(?m)^Пока нет новых необработанных сообщений\.\s*', ''
$inbox = [regex]::Replace(
    $inbox,
    '(?ms)(^## Новые сообщения\s*\r?\n)(.*?)(\r?\n## Обработанные входящие)',
    {
        param($match)

        $existing = $match.Groups[2].Value.Trim()
        $body = if ([string]::IsNullOrWhiteSpace($existing)) {
            "`r`n$entry"
        } else {
            "`r`n$entry`r`n$existing`r`n"
        }

        return $match.Groups[1].Value + $body.TrimEnd() + $match.Groups[3].Value
    },
    1
)

Set-Content -LiteralPath $inboxPath -Encoding UTF8 -Value $inbox

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Accepted message into inbox: $Title"
}
