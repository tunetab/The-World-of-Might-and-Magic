param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^Q-(?:C\d+|WORLD)-\d{3}$')]
    [string]$QuestionId,

    [string]$Resolution = 'Закрыто решением или обновлением канона.',

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

$registryText = Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath
$registry = $registryText | ConvertFrom-Json
$questions = @($registry.questions)
$question = $questions | Where-Object { $_.id -eq $QuestionId } | Select-Object -First 1

if ($null -eq $question) {
    throw "Cannot find $QuestionId in 09_Реестры/Вопросы.json."
}

if ($question.status -eq 'resolved') {
    throw "$QuestionId is already resolved in 09_Реестры/Вопросы.json."
}

$question.status = 'resolved'
$registry.updated_real_date = $today

$history = @($registry.history)
$history += [pscustomobject][ordered]@{
    date = $today
    id = $QuestionId
    resolution = $Resolution
}
$registry.history = @($history)

# Preserve the old archive behavior: newly closed questions appear at the end of the relevant closed table.
$registry.questions = @(
    $questions | Where-Object { $_.id -ne $QuestionId }
) + @($question)

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

"Closed question: $QuestionId"
}
