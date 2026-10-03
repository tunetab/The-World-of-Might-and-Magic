param(
    [string]$Title = '',

    [string]$RequestId = '',

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

    return @(Get-WmmaInboxEntries $NewBody)
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

function Resolve-SourceReference {
    param(
        [string]$Root,
        [string]$SourceLine
    )

    if ([string]::IsNullOrWhiteSpace($SourceLine)) {
        return $null
    }

    $sourceReference = $SourceLine.Trim()
    if ($sourceReference -match '`([^`]+\.md)`') {
        $sourceReference = $Matches[1]
    }

    if ($sourceReference -match '^[a-z]+://' -or $sourceReference -notmatch '\.md$') {
        return $null
    }

    $normalized = $sourceReference.Trim('`') -replace '/', '\'
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        return $null
    }

    $candidatePath = $normalized
    if (-not [System.IO.Path]::IsPathRooted($candidatePath)) {
        $candidatePath = Join-Path $Root $normalized
    }

    if (-not (Test-Path -LiteralPath $candidatePath)) {
        return $null
    }

    return (Resolve-Path -LiteralPath $candidatePath).Path
}

function Set-SourceLifecycleStatus {
    param(
        [string]$Root,
        [string]$SourceLine,
        [string]$LifecycleStatus
    )

    if ([string]::IsNullOrWhiteSpace($LifecycleStatus)) {
        return
    }

    $sourcePath = Resolve-SourceReference -Root $Root -SourceLine $SourceLine
    if (-not $sourcePath) {
        return
    }

    $sourceRoot = (Resolve-Path -LiteralPath (Join-Path $Root '08_Источники')).Path
    $isInSourceRoot = $sourcePath.StartsWith($sourceRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
    if (-not $isInSourceRoot) {
        return
    }

    $sourceText = Get-Content -Raw -Encoding UTF8 -LiteralPath $sourcePath
    if ($sourceText -notmatch '(?m)^type:\s*(source|source_note|source_compilation)\s*$') {
        return
    }

    if ($sourceText -notmatch '(?m)^status:\s*\S+') {
        return
    }

    $updatedSourceText = Set-WmmaMeta $sourceText 'status' $LifecycleStatus

    if ($updatedSourceText -ne $sourceText) {
        $encoding = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($sourcePath, $updatedSourceText, $encoding)
    }
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$inbox = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
$codeFence = '```'

$sections=@(Get-WmmaMarkdownSections $inbox 2)
$newSection=@($sections|Where-Object heading -eq 'Новые сообщения')|Select-Object -First 1
$processedSection=@($sections|Where-Object heading -eq 'Обработанные входящие')|Select-Object -First 1
if (-not $newSection -or -not $processedSection -or $newSection.index -ge $processedSection.index) {
    throw 'Inbox structure is broken: expected "## Новые сообщения" and "## Обработанные входящие".'
}

$newHeader = $inbox.Substring(0,$newSection.body_start).TrimEnd()
$newBody = $newSection.text
$processedHeader = "`r`n`r`n## Обработанные входящие"
$processedRest = $inbox.Substring($processedSection.body_start).TrimStart()

$entries = Get-InboxEntries -NewBody $newBody
if($RequestId){
    $receipt=Get-WmmaReceipt $root $RequestId
    if($receipt){
        Assert-WmmaReceiptPayload $root $receipt
        $targetState=if($Status -eq 'обработано'){'processed'}elseif($Status -eq 'отклонено'){'archived'}else{'deferred'}
        if($receipt.state -in @('processed','archived')){
            if($receipt.state -cne $targetState -or ($ScenePath -and $receipt.scene_path -cne $ScenePath)){throw 'Request already completed with another result.'}
            "Processed inbox message: $RequestId (already completed)";return
        }
    }
    $matching=@($entries|Where-Object {(Get-WmmaEntryField $_.Value 'Request-ID') -ceq $RequestId})
    if($matching.Count -ne 1){throw 'Request ID must select exactly one new message.'}
    $selected=$matching[0]
}else{$selected = Select-InboxEntry -Entries $entries -Needle $Title -UseFirst:$First}
$selectedHeading = $selected.Groups[1].Value.Trim()
$selectedBody = $selected.Groups[2].Value.Trim()
$requestId=Get-WmmaEntryField $selectedBody 'Request-ID'
if($requestId){$receipt=Get-WmmaReceipt $root $requestId;if($receipt){Assert-WmmaReceiptPayload $root $receipt}}
foreach($reference in @($ScenePath)+@($SourcePath)+@($Links)|Where-Object {$_}){
    $reference=$reference.Trim([char]96)
    if($reference -match '^[a-z]+://'){continue}
    if(-not (Test-Path -LiteralPath (Resolve-WmmaPath $root $reference))){throw "Related file is missing: $reference"}
}
if($Status -eq 'отложено'){
    $replacement=[regex]::new('(?m)^Статус:[^\r\n]*').Replace($selected.Value,'Статус: отложено.',1)
    $start=$newSection.body_start+$selected.Index
    Write-WmmaText $inboxPath ($inbox.Substring(0,$start)+$replacement+$inbox.Substring($start+$selected.Length))
    if($requestId){$receipt=Get-WmmaReceipt $root $requestId;if($receipt){$receipt.state='deferred';Save-WmmaReceipt $root $receipt}}
    if(-not $SkipCheck){& (Join-Path $root 'tools/Завершить_ход.ps1')}
    "Deferred inbox message: $selectedHeading";return
}

if (-not [string]::IsNullOrWhiteSpace($SourcePath)) {
    $sourceLine = Format-ProjectReference -Value $SourcePath
} elseif (Get-WmmaEntryField $selectedBody 'Источник') {
    $sourceLine = Get-WmmaEntryField $selectedBody 'Источник'
} else {
    $sourceLine = 'не указан'
}

$sourceLifecycleStatus = switch ($Status) {
    'обработано' { 'processed' }
    'отклонено' { 'archived' }
    default { $null }
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
if($requestId){$processedLines.Add("Request-ID: $requestId")|Out-Null}

if ($relatedLinks.Count -gt 0) {
    $processedLines.Add("Связано: $($relatedLinks -join ', ')") | Out-Null
}

$processedLines.Add('') | Out-Null
$processedLines.Add('Кратко:') | Out-Null
$processedLines.Add('') | Out-Null
$processedLines.Add((Convert-ToBulletText -Value $Summary)) | Out-Null

$hasSourceFile = $sourceLine -match '`[^`]+\.md`'
$rawMessage=Get-WmmaRawMessage $selectedBody
if (-not $hasSourceFile -and $null -ne $rawMessage) {
    $codeFence=Get-WmmaTextFence $rawMessage
    $processedLines.Add('') | Out-Null
    $processedLines.Add('Исходное входящее:') | Out-Null
    $processedLines.Add('') | Out-Null
    $processedLines.Add("${codeFence}text") | Out-Null
    $processedLines.Add($rawMessage) | Out-Null
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

Write-WmmaText $inboxPath $updatedInbox
Set-SourceLifecycleStatus -Root $root -SourceLine $sourceLine -LifecycleStatus $sourceLifecycleStatus
if($requestId){
    $receipt=Get-WmmaReceipt $root $requestId
    if($receipt){$receipt.state=if($Status -eq 'обработано'){'processed'}else{'archived'};if($ScenePath){$receipt.scene_path=$ScenePath};Save-WmmaReceipt $root $receipt}
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}

"Processed inbox message: $selectedHeading"
}
