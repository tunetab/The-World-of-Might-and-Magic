param(
    [Parameter(Mandatory = $true)]
    [string]$Title,

    [string]$Text = '',

    [string]$TextPath = '',

    [ValidateSet('inbox', 'source')]
    [string]$Mode = 'source',

    [string]$RequestId = '',

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
Assert-WmmaSingleLine $Title 'Title'

if (-not [string]::IsNullOrWhiteSpace($TextPath)) {
    $resolvedTextPath = (Resolve-Path -LiteralPath $TextPath).Path
    $Text = Get-Content -Raw -Encoding UTF8 -LiteralPath $resolvedTextPath
}

if ([string]::IsNullOrWhiteSpace($Text)) {
    throw 'Provide message text through -Text or -TextPath.'
}
$codeFence=Get-WmmaTextFence $Text

$sourceReference = 'вручную через `tools/Принять_сообщение.ps1`'
$messageHash=Get-WmmaHash ($Text.Replace("`r`n","`n").Trim())
if(-not $RequestId){$RequestId='MSG-'+$messageHash.Substring(0,24)}
if($RequestId -notmatch '^[A-Za-z0-9-]+$'){throw 'Invalid request ID.'}
$receipt=Get-WmmaReceipt $root $RequestId
if($receipt){
    if($receipt.content_sha256 -cne $messageHash){throw 'Request ID already belongs to different content.'}
    Assert-WmmaReceiptPayload $root $receipt
    "Accepted message into inbox: $($receipt.title) (already recorded: $RequestId)"
    return
}
$receipt=[pscustomobject]@{request_id=$RequestId;content_sha256=$messageHash;title=$Title;source_path=$null;scene_path=$null;state='accepted'}

if ($Mode -eq 'source') {
    $sourceRoot = Join-Path $root '08_Источники'
    $sourceFileName = "$today`_$(Convert-WmmaFileName -Lowercase -Value $Title).md"
    $sourcePath = Join-Path $sourceRoot $sourceFileName
    if((Test-Path -LiteralPath $sourcePath) -and (Get-WmmaMeta (Read-WmmaText $sourcePath) 'request_id') -ne $RequestId){
        $sourceFileName="$today`_$(Convert-WmmaFileName -Lowercase -Value $Title)_$RequestId.md"
        $sourcePath=Join-Path $sourceRoot $sourceFileName
    }

    $reuseSource=$false
    if (Test-Path -LiteralPath $sourcePath) {
        $existingSource=Read-WmmaText $sourcePath
        if((Get-WmmaMeta $existingSource 'request_id') -ne $RequestId -or (Get-WmmaMeta $existingSource 'content_sha256') -cne $messageHash){throw "Source file already exists for another message: $(Get-WmmaRelativePath -Root $root $sourcePath)"}
        if(-not (Test-WmmaSourcePayload $existingSource $messageHash)){throw 'Existing source payload differs from the original message.'}
        $reuseSource=$true
    }

    $sourceContent = @"
# $Title

---
type: source_note
status: new
canon_level: draft
received_real_date: $today
id: SRC-$([guid]::NewGuid().ToString('N'))
request_id: $RequestId
content_sha256: $messageHash
---

${codeFence}text
$Text
${codeFence}
"@

    if(-not $reuseSource){Write-WmmaText $sourcePath $sourceContent}
    $sourceReference = "``$(Get-WmmaRelativePath -Root $root $sourcePath)``"
    $receipt.source_path=Get-WmmaRelativePath -Root $root $sourcePath
}

$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$inbox = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
if(@(Get-WmmaInboxEntries $inbox|Where-Object {(Get-WmmaEntryField $_.Value 'Request-ID') -ceq $RequestId}).Count -gt 0){
    Save-WmmaReceipt $root $receipt
    "Accepted message into inbox: $Title (recovered: $RequestId)";return
}
$entry = @"
### $today. $Title

Статус: новое.
Request-ID: $RequestId
Источник: $sourceReference

${codeFence}text
$Text
${codeFence}

"@

if ($inbox -notmatch '(?m)^## Новые сообщения\s*$') {
    throw 'Inbox section not found: ## Новые сообщения'
}

$newSection=@(Get-WmmaMarkdownSections $inbox 2|Where-Object heading -eq 'Новые сообщения')|Select-Object -First 1
if(-not $newSection){throw 'Inbox new-message section is missing.'}
$existing=$newSection.text.Trim()
if($existing -eq 'Пока нет новых необработанных сообщений.'){$existing=''}
$replacement="`n$entry`n$existing`n`n"
$inbox=$inbox.Substring(0,$newSection.body_start)+$replacement+$inbox.Substring($newSection.end)

Write-WmmaText $inboxPath $inbox
Save-WmmaReceipt $root $receipt

if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}

"Accepted message into inbox: $Title"
}
