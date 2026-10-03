param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^DEC-PENDING-\d{3}$')]
    [string]$PendingId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^DEC-\d{3}$')]
    [string]$AcceptedId,

    [Parameter(Mandatory = $true)]
    [string]$Choice,

    [string]$Effect = 'Уточнить последствия в связанных файлах.',

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$registryPath = Join-Path $root '09_Реестры\Решения.json'

if (-not (Test-Path -LiteralPath $registryPath)) {
    throw "Decision registry is missing: 09_Реестры/Решения.json. Restore the authoritative JSON registry from a verified backup."
}

$registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $registryPath) | ConvertFrom-Json
$decisions = @($registry.decisions)
$pendingDecision = $decisions | Where-Object { $_.id -eq $PendingId } | Select-Object -First 1

if ($null -eq $pendingDecision) {
    $already=@($decisions | Where-Object {$_.resolved_from -eq $PendingId -and $_.id -eq $AcceptedId -and $_.state -eq 'accepted'})|Select-Object -First 1
    if($already -and $already.choice -ceq $Choice -and $already.immediate_effect -ceq $Effect){"Closed decision: $PendingId -> $AcceptedId (already applied)";return}
    throw "Cannot find $PendingId in 09_Реестры/Решения.json."
}

if ($pendingDecision.state -ne 'pending' -and $pendingDecision.id -notlike 'DEC-PENDING-*') {
    throw "$PendingId is not pending in 09_Реестры/Решения.json."
}

if ($decisions | Where-Object { $_.id -eq $AcceptedId } | Select-Object -First 1) {
    throw "Accepted decision ID already exists in 09_Реестры/Решения.json: $AcceptedId"
}

$pendingDecision | Add-Member -NotePropertyName uid -NotePropertyValue $(if ($pendingDecision.uid) {$pendingDecision.uid} else {'DUID-' + [guid]::NewGuid().ToString('N')}) -Force
$pendingDecision | Add-Member -NotePropertyName resolved_from -NotePropertyValue $PendingId -Force
$pendingDecision | Add-Member -NotePropertyName original_question -NotePropertyValue $pendingDecision.question -Force
$pendingDecision | Add-Member -NotePropertyName transitions -NotePropertyValue @(@($pendingDecision.transitions | Where-Object {$_}) + [pscustomobject]@{from=$PendingId;to=$AcceptedId;at=$today}) -Force
$pendingDecision.id = $AcceptedId
$pendingDecision.state = 'accepted'
$pendingDecision.real_date = $today
$pendingDecision.choice = $Choice
$pendingDecision.immediate_effect = $Effect
$pendingDecision.status_text = 'принято.'
$pendingDecision.priority = $null
$pendingDecision.question = $null
$pendingDecision.owner = $null
$pendingDecision.panel_status = $null
$registry.updated_real_date = $today

# Preserve the old log behavior: newly accepted decisions appear at the end of the accepted section.
$registry.decisions = @(
    $decisions | Where-Object { $_.id -ne $PendingId -and $_.id -ne $AcceptedId }
) + @($pendingDecision)

$json = ($registry | ConvertTo-Json -Depth 8).TrimEnd() + "`n"
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($registryPath, $json, $encoding)

$contextPath = Join-Path $root '01_Кампания\00_Текущий_контекст.md'
if (Test-Path -LiteralPath $contextPath) {
    $contextLines = Get-Content -Encoding UTF8 -LiteralPath $contextPath
    # Only the numbered decision entry is removed. Narrative paragraphs survive;
    # their reference is updated to the accepted ID without deleting other facts.
    $contextLines = @($contextLines | ForEach-Object {
        if ($_ -match ('^\d+\.\s+`' + [regex]::Escape($PendingId) + '`')) { return }
        $_.Replace($PendingId, $AcceptedId)
    })
    $renumbered = New-Object 'System.Collections.Generic.List[string]'
    $insideImmediate = $false
    $counter = 1

    foreach ($line in $contextLines) {
        if ($line -eq '## Немедленные решения') {
            $insideImmediate = $true
            $counter = 1
            $renumbered.Add($line) | Out-Null
            continue
        }

        if ($insideImmediate -and $line -match '^##\s+') {
            $insideImmediate = $false
        }

        if ($insideImmediate -and $line -match '^\d+\.\s+`DEC-PENDING-\d{3}`') {
            $renumbered.Add(($line -replace '^\d+\.', "$counter.")) | Out-Null
            $counter++
        } else {
            $renumbered.Add($line) | Out-Null
        }
    }

    [System.IO.File]::WriteAllText($contextPath, (($renumbered -join "`n").TrimEnd() + "`n"), $encoding)
}

& (Join-Path $root 'tools\Собрать_решения.ps1') -SkipCheck
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}

"Closed decision: $PendingId -> $AcceptedId"
}
