param(
    [ValidateSet('chapter', 'world')]
    [string]$Scope = 'chapter',

    [Parameter(Mandatory = $true)]
    [string]$Text,

    [Parameter(Mandatory = $true)]
    [string]$Owner,

    [ValidateSet('критический', 'высокий', 'средний', 'низкий')]
    [string]$Priority = 'высокий',

    [ValidateSet('active', 'waiting', 'later')]
    [string]$Status = 'waiting',

    [ValidatePattern('^Q-(?:C2|WORLD)-\d{3}$')]
    [string]$Id,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$registryPath = Join-Path $root '09_Реестры\Вопросы.json'

if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "Question registry is missing: 09_Реестры/Вопросы.json. Run .\tools\Собрать_вопросы.ps1 -ImportFromMarkdown once."
}

function Get-QuestionPrefix {
    param(
        [object]$Registry,
        [string]$QuestionScope
    )

    if ($QuestionScope -eq 'world') {
        return 'Q-WORLD'
    }

    if ($null -eq $Registry.current_chapter) {
        throw 'Question registry has no current_chapter for chapter question ID generation.'
    }

    return "Q-C$($Registry.current_chapter)"
}

function Get-NextQuestionId {
    param(
        [object]$Registry,
        [string]$QuestionScope
    )

    $prefix = Get-QuestionPrefix -Registry $Registry -QuestionScope $QuestionScope
    $escapedPrefix = [regex]::Escape($prefix)
    $numbers = @(
        @($Registry.questions) + @($Registry.history) |
            ForEach-Object {
                if ($_.id -match "^$escapedPrefix-(\d{3})$") {
                    [int]$Matches[1]
                }
            }
    )

    $max = 0
    if ($numbers.Count -gt 0) {
        $max = [int](($numbers | Measure-Object -Maximum).Maximum)
    }

    return ('{0}-{1:D3}' -f $prefix, ($max + 1))
}

$registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath) | ConvertFrom-Json
$questions = @($registry.questions)
$history = @($registry.history)
$expectedPrefix = Get-QuestionPrefix -Registry $registry -QuestionScope $Scope

if ([string]::IsNullOrWhiteSpace($Id)) {
    $Id = Get-NextQuestionId -Registry $registry -QuestionScope $Scope
} elseif ($Id -notlike "$expectedPrefix-*") {
    throw "Question ID $Id does not match scope '$Scope'. Expected prefix: $expectedPrefix"
}

if (@($questions | Where-Object { $_.id -eq $Id }).Count -gt 0) {
    throw "Question ID already exists in 09_Реестры/Вопросы.json: $Id"
}

if (@($history | Where-Object { $_.id -eq $Id }).Count -gt 0) {
    throw "Question ID already exists in question history: $Id"
}

$questions += [pscustomobject][ordered]@{
    id = $Id
    priority = $Priority
    text = $Text
    owner = $Owner
    scope = $Scope
    status = $Status
}

$registry.questions = @($questions)
$registry.updated_real_date = $today

$json = ($registry | ConvertTo-Json -Depth 8).TrimEnd() + "`n"
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($registryPath, $json, $encoding)

& (Join-Path $root 'tools\Собрать_вопросы.ps1') -SkipCheck
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Собрать_панель_хода.ps1') -SkipCheck
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

"Created question: $Id"
}
