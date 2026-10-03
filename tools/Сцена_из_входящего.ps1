param(
    [Parameter(Mandatory = $true)]
    [string]$Branch,

    [string]$Title = '',

    [string]$Text = '',

    [string]$TextPath = '',

    [string]$SceneTitle = '',

    [int]$Chapter = 0,

    [ValidateSet('draft', 'active', 'closed')]
    [string]$Status = 'draft',

    [string]$CanonLevel = 'draft',

    [string]$DateInStory = 'Уточнить.',

    [string]$Location = 'Уточнить.',

    [string]$FrontId = '-',

    [string]$SceneSummary = '',

    [string]$ProcessSummary = '',

    [switch]$FirstInbox,

    [switch]$NoSource,

    [string]$RequestId = '',

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
function Get-NewInboxEntries {
    param([string]$InboxText)

    $section=@(Get-WmmaMarkdownSections $InboxText 2|Where-Object heading -eq 'Новые сообщения')|Select-Object -First 1
    if (-not $section) {
        throw 'Inbox structure is broken.'
    }

    return @(Get-WmmaInboxEntries $section.text)
}

function Select-InboxHeading {
    param(
        [object[]]$Entries,
        [string]$Needle,
        [switch]$UseFirst
    )

    if ($Entries.Count -eq 0) {
        throw 'No new inbox messages found.'
    }

    if ($UseFirst) {
        return $Entries[0].Groups[1].Value.Trim()
    }

    if ([string]::IsNullOrWhiteSpace($Needle)) {
        throw 'Provide -Title, -Text/-TextPath, or use -FirstInbox.'
    }

    $matches = @(
        $Entries | Where-Object {
            $heading = $_.Groups[1].Value.Trim()
            $plainHeading = $heading -replace '^\d{4}-\d{2}-\d{2}\.\s*', ''
            $heading.Equals($Needle, [System.StringComparison]::OrdinalIgnoreCase) -or
                $plainHeading.Equals($Needle, [System.StringComparison]::OrdinalIgnoreCase) -or
                $heading.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        }
    )
    $exact=@($Entries|Where-Object {($_.Groups[1].Value.Trim() -replace '^\d{4}-\d{2}-\d{2}\.\s*','') -ceq $Needle -or $_.Groups[1].Value.Trim() -ceq $Needle})
    if($exact.Count -eq 1){return $exact[0].Groups[1].Value.Trim()}

    if ($matches.Count -ne 1) {
        throw "Inbox message match count is $($matches.Count) for: $Needle"
    }

    return $matches[0].Groups[1].Value.Trim()
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$hasInlineMessage = -not [string]::IsNullOrWhiteSpace($Text) -or -not [string]::IsNullOrWhiteSpace($TextPath)
if($TextPath){$Text=Read-WmmaText ((Resolve-Path -LiteralPath $TextPath).Path)}
if($hasInlineMessage -and -not $RequestId){$RequestId='MSG-'+(Get-WmmaHash ($Text.Replace("`r`n","`n").Trim())).Substring(0,24)}
$receipt=if($RequestId){Get-WmmaReceipt $root $RequestId}else{$null}
if($receipt){
    Assert-WmmaReceiptPayload $root $receipt
    if($hasInlineMessage -and $receipt.content_sha256 -cne (Get-WmmaHash ($Text.Replace("`r`n","`n").Trim()))){throw 'Request ID content mismatch.'}
    if($receipt.scene_path){
        $scene=Read-WmmaText (Resolve-WmmaPath $root $receipt.scene_path)
        if((Get-WmmaMeta $scene 'branch') -cne $Branch -or ($Chapter -gt 0 -and (Get-WmmaMeta $scene 'chapter') -ne [string]$Chapter)){throw 'Request ID belongs to another scene branch or chapter.'}
    }
}
if($receipt -and $receipt.state -eq 'scene_created' -and $receipt.scene_path){
    $recordedInbox=Read-WmmaText (Join-Path $root '07_Черновики_и_идеи/Входящие_сообщения.md')
    $processedPart=Get-WmmaSection $recordedInbox 'Обработанные входящие'
    $processedEntry=@(Get-WmmaInboxEntries $processedPart|Where-Object {(Get-WmmaEntryField $_.Value 'Request-ID') -ceq $RequestId -and (Get-WmmaEntryField $_.Value 'Связано').Contains($receipt.scene_path)})
    if($processedEntry.Count -eq 1){$receipt.state='processed';Save-WmmaReceipt $root $receipt}
}
if($receipt -and $receipt.state -eq 'processed' -and $receipt.scene_path){
    if($hasInlineMessage -and $receipt.content_sha256 -cne (Get-WmmaHash ($Text.Replace("`r`n","`n").Trim()))){throw 'Request ID content mismatch.'}
    "Created scene from inbox: $($receipt.scene_path)";return
}

if ($hasInlineMessage) {
    if ([string]::IsNullOrWhiteSpace($Title)) {
        throw 'Inline message mode requires -Title.'
    }

    $acceptArgs = @{
        Title = $Title
        Mode = if ($NoSource) { 'inbox' } else { 'source' }
        SkipCheck = $true
        RequestId = $RequestId
    }

    if (-not [string]::IsNullOrWhiteSpace($TextPath)) {
        $acceptArgs.TextPath = $TextPath
    } else {
        $acceptArgs.Text = $Text
    }

    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Принять_сообщение.ps1') @acceptArgs
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

$inboxPath = Join-Path $root '07_Черновики_и_идеи\Входящие_сообщения.md'
$inbox = Get-Content -Raw -Encoding UTF8 -LiteralPath $inboxPath
$inboxEntries = Get-NewInboxEntries -InboxText $inbox
if($RequestId){
    $matching=@($inboxEntries|Where-Object {(Get-WmmaEntryField $_.Value 'Request-ID') -ceq $RequestId})
    if($matching.Count -ne 1){throw 'Request ID must select exactly one new message.'}
    $selectedHeading=$matching[0].Groups[1].Value.Trim()
}else{$selectedHeading = Select-InboxHeading -Entries $inboxEntries -Needle $Title -UseFirst:$FirstInbox}
$selectedEntry=@($inboxEntries|Where-Object {$_.Groups[1].Value.Trim() -eq $selectedHeading})[0]
if($RequestId){$selectedEntry=$matching[0]}
if(-not $RequestId){$RequestId=Get-WmmaEntryField $selectedEntry.Value 'Request-ID'}
$receipt=if($RequestId){Get-WmmaReceipt $root $RequestId}else{$null}
if($receipt){Assert-WmmaReceiptPayload $root $receipt}
$sourceIds=@()
if($receipt -and $receipt.source_path){$sourceIds=@(Get-WmmaMeta (Read-WmmaText (Join-Path $root $receipt.source_path)) 'id')}

if ([string]::IsNullOrWhiteSpace($Title)) {
    $Title = $selectedHeading -replace '^\d{4}-\d{2}-\d{2}\.\s*', ''
}

if ([string]::IsNullOrWhiteSpace($SceneTitle)) {
    $SceneTitle = $Title
}

if ([string]::IsNullOrWhiteSpace($SceneSummary)) {
    $SceneSummary = "Сцена создана из входящего сообщения: $Title."
}

$sceneArgs = @{
    Branch = $Branch
    Title = $SceneTitle
    Chapter = $Chapter
    Status = $Status
    CanonLevel = $CanonLevel
    DateInStory = $DateInStory
    Location = $Location
    FrontId = $FrontId
    Summary = $SceneSummary
    SkipCheck = $true
}
if($RequestId){$sceneArgs.RequestId=$RequestId}
$sceneArgs.SourceIds=$sourceIds

$global:LASTEXITCODE = 0
$sceneOutput = & (Join-Path $root 'tools\Новая_сцена.ps1') @sceneArgs
if (-not $? -or $LASTEXITCODE -ne 0) {
    exit 1
}

$createdScene = $null
foreach ($line in $sceneOutput) {
    if ($line -match '^Created scene:\s*(.+?)\s*$') {
        $createdScene = $Matches[1].Trim()
    }
}

if (-not $createdScene) {
    throw 'Could not determine created scene path.'
}
if($receipt){$receipt.scene_path=$createdScene;$receipt.state='scene_created';Save-WmmaReceipt $root $receipt}

if ([string]::IsNullOrWhiteSpace($ProcessSummary)) {
    $ProcessSummary = "Создана новая сцена `$createdScene`; дальнейшая обработка канона ведется в этой сцене."
}

$processArgs = @{
    Title = $Title
    Summary = $ProcessSummary
    ScenePath = $createdScene
    SkipCheck = $true
}
if($RequestId){$processArgs.RequestId=$RequestId}

$global:LASTEXITCODE = 0
& (Join-Path $root 'tools\Обработать_входящее.ps1') @processArgs
if (-not $? -or $LASTEXITCODE -ne 0) {
    exit 1
}
if($receipt){$receipt.state='processed';Save-WmmaReceipt $root $receipt}

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Завершить_ход.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Created scene from inbox: $createdScene"
}
