param(
    [Parameter(Mandatory = $true)]
    [string]$Branch,

    [string]$Title = '',

    [string]$Text = '',

    [string]$TextPath = '',

    [string]$SceneTitle = '',

    [int]$Chapter = 2,

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

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
function Get-NewInboxEntries {
    param([string]$InboxText)

    $section = [regex]::Match(
        $InboxText,
        '(?ms)\A.*?^## Новые сообщения\s*\r?\n(.*?)(?:\r?\n## Обработанные входящие\s*\r?\n).*\z'
    )

    if (-not $section.Success) {
        throw 'Inbox structure is broken.'
    }

    return @([regex]::Matches($section.Groups[1].Value, '(?ms)^###\s+(.+?)\s*\r?\n(.*?)(?=^###\s+|\z)'))
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

    if ($matches.Count -ne 1) {
        throw "Inbox message match count is $($matches.Count) for: $Needle"
    }

    return $matches[0].Groups[1].Value.Trim()
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$hasInlineMessage = -not [string]::IsNullOrWhiteSpace($Text) -or -not [string]::IsNullOrWhiteSpace($TextPath)

if ($hasInlineMessage) {
    if ([string]::IsNullOrWhiteSpace($Title)) {
        throw 'Inline message mode requires -Title.'
    }

    $acceptArgs = @{
        Title = $Title
        Mode = if ($NoSource) { 'inbox' } else { 'source' }
        SkipCheck = $true
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
$selectedHeading = Select-InboxHeading -Entries $inboxEntries -Needle $Title -UseFirst:$FirstInbox

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

if ([string]::IsNullOrWhiteSpace($ProcessSummary)) {
    $ProcessSummary = "Создана новая сцена `$createdScene`; дальнейшая обработка канона ведется в этой сцене."
}

$processArgs = @{
    Title = $Title
    Summary = $ProcessSummary
    ScenePath = $createdScene
    SkipCheck = $true
}

$global:LASTEXITCODE = 0
& (Join-Path $root 'tools\Обработать_входящее.ps1') @processArgs
if (-not $? -or $LASTEXITCODE -ne 0) {
    exit 1
}

if (-not $SkipCheck) {
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'tools\Завершить_ход.ps1')
    if (-not $? -or $LASTEXITCODE -ne 0) {
        exit 1
    }
}

"Created scene from inbox: $createdScene"
}
