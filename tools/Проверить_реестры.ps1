param(
    [switch]$Quiet,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$errors = New-Object 'System.Collections.Generic.List[string]'
$warnings = New-Object 'System.Collections.Generic.List[string]'

function Add-Problem {
    param(
        [string]$Kind,
        [string]$Message
    )

    if ($Kind -eq 'Error') {
        $errors.Add($Message) | Out-Null
    } else {
        $warnings.Add($Message) | Out-Null
    }
}

function Get-RelativePath {
    param([string]$Path)

    if ($Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($root.Length).TrimStart('\', '/')
    }

    return $Path
}

function Test-HasProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $false
    }

    return @($Object.PSObject.Properties.Match($Name)).Count -gt 0
}

function Get-PropertyValue {
    param(
        [object]$Object,
        [string]$Name
    )

    if (-not (Test-HasProperty -Object $Object -Name $Name)) {
        return $null
    }

    return $Object.$Name
}

function Test-IsBlank {
    param([object]$Value)

    return ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value))
}

function Test-ProjectPath {
    param([string]$Reference)

    if ([string]::IsNullOrWhiteSpace($Reference)) {
        return $false
    }

    $cleanReference = $Reference.Trim().Trim('`')
    if ($cleanReference -match '^[a-z][a-z0-9+.-]*://') {
        return $true
    }

    $normalized = $cleanReference -replace '/', '\'

    if ([System.IO.Path]::IsPathRooted($normalized)) {
        return Test-Path -LiteralPath $normalized
    }

    return Test-Path -LiteralPath (Join-Path $root $normalized)
}

function Assert-RequiredTextField {
    param(
        [object]$Object,
        [string]$Field,
        [string]$Context
    )

    if (-not (Test-HasProperty -Object $Object -Name $Field)) {
        Add-Problem Error "$Context has no required field: $Field"
        return
    }

    if (Test-IsBlank (Get-PropertyValue -Object $Object -Name $Field)) {
        Add-Problem Error "$Context has empty required field: $Field"
    }
}

function Assert-AllowedValue {
    param(
        [object]$Object,
        [string]$Field,
        [string[]]$Allowed,
        [string]$Context
    )

    if (-not (Test-HasProperty -Object $Object -Name $Field)) {
        Add-Problem Error "$Context has no required field: $Field"
        return
    }

    $value = [string](Get-PropertyValue -Object $Object -Name $Field)
    if ($Allowed -notcontains $value) {
        Add-Problem Error "$Context has invalid ${Field}: $value"
    }
}

function Assert-LocalReference {
    param(
        [string]$Reference,
        [string]$Context
    )

    if (-not (Test-ProjectPath -Reference $Reference)) {
        Add-Problem Error "$Context points to missing file: $Reference"
    }
}

function Get-RegistryArray {
    param(
        [object]$Registry,
        [string]$Field,
        [string]$Context,
        [switch]$AllowEmpty
    )

    if (-not (Test-HasProperty -Object $Registry -Name $Field)) {
        Add-Problem Error "$Context has no required array: $Field"
        return @()
    }

    $value = Get-PropertyValue -Object $Registry -Name $Field
    if ($null -eq $value) {
        Add-Problem Error "$Context has null array: $Field"
        return @()
    }

    $items = @($value)
    if (-not $AllowEmpty -and $items.Count -eq 0) {
        Add-Problem Error "$Context has empty array: $Field"
    }

    return $items
}

function Assert-UniqueValues {
    param(
        [string[]]$Values,
        [string]$Context
    )

    $duplicates = @(
        $Values |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Group-Object |
            Where-Object { $_.Count -gt 1 } |
            ForEach-Object { $_.Name }
    )

    foreach ($duplicate in $duplicates) {
        Add-Problem Error "$Context contains duplicate value: $duplicate"
    }
}

function Assert-LinkText {
    param(
        [object]$Object,
        [string]$Field,
        [string]$Context
    )

    Assert-RequiredTextField -Object $Object -Field $Field -Context $Context
    $value = [string](Get-PropertyValue -Object $Object -Name $Field)
    if ([string]::IsNullOrWhiteSpace($value)) {
        return
    }

    $matches = [regex]::Matches($value, '`([^`]+)`')
    if ($matches.Count -eq 0) {
        Add-Problem Error "$Context has no backticked local references in field: $Field"
        return
    }

    foreach ($match in $matches) {
        Assert-LocalReference -Reference $match.Groups[1].Value -Context "$Context field $Field"
    }
}

function Assert-LinkArray {
    param(
        [object]$Object,
        [string]$Field,
        [string]$Context
    )

    $links = Get-RegistryArray -Registry $Object -Field $Field -Context $Context
    foreach ($link in $links) {
        if (Test-IsBlank $link) {
            Add-Problem Error "$Context contains empty link in field: $Field"
            continue
        }

        Assert-LocalReference -Reference ([string]$link) -Context "$Context field $Field"
    }
}

function Assert-GeneratedFrom {
    param(
        [object]$Registry,
        [string]$Context
    )

    $sources = Get-RegistryArray -Registry $Registry -Field 'generated_from' -Context $Context
    foreach ($source in $sources) {
        if (Test-IsBlank $source) {
            Add-Problem Error "$Context has empty generated_from entry."
            continue
        }

        Assert-LocalReference -Reference ([string]$source) -Context "$Context generated_from"
    }
}

function Assert-ViewCount {
    param(
        [object]$View,
        [object[]]$Items,
        [string]$Context
    )

    Assert-RequiredTextField -Object $View -Field 'count' -Context $Context
    $countValue = Get-PropertyValue -Object $View -Name 'count'
    if ($null -eq $countValue) {
        return
    }

    try {
        $declaredCount = [int]$countValue
    } catch {
        Add-Problem Error "$Context has non-numeric count: $countValue"
        return
    }

    if ($declaredCount -ne @($Items).Count) {
        Add-Problem Error "$Context count $declaredCount does not match actual item count $(@($Items).Count)."
    }
}

function Assert-SourceRegistry {
    param(
        [object]$View,
        [string]$ExpectedSource,
        [string]$Context
    )

    $sources = @((Get-PropertyValue -Object $View -Name 'generated_from') | ForEach-Object { [string]$_ })
    if ($sources -notcontains $ExpectedSource) {
        Add-Problem Error "$Context generated_from does not include expected source: $ExpectedSource"
    }
}

function Get-ObjectIds {
    param([object[]]$Items)

    return @(
        $Items |
            ForEach-Object { [string](Get-PropertyValue -Object $_ -Name 'id') } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
}

function Compare-IdSets {
    param(
        [string]$LeftName,
        [string[]]$LeftIds,
        [string]$RightName,
        [string[]]$RightIds
    )

    $leftUnique = @($LeftIds | Sort-Object -Unique)
    $rightUnique = @($RightIds | Sort-Object -Unique)

    foreach ($id in $leftUnique) {
        if ($rightUnique -notcontains $id) {
            Add-Problem Error "$LeftName contains $id but $RightName does not."
        }
    }

    foreach ($id in $rightUnique) {
        if ($leftUnique -notcontains $id) {
            Add-Problem Error "$RightName contains $id but $LeftName does not."
        }
    }
}

function Read-RegistryJson {
    param(
        [string]$RelativePath,
        [string]$ExpectedType,
        [string[]]$ExtraTopFields = @()
    )

    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Problem Error "Registry file is missing: $RelativePath"
        return $null
    }

    try {
        $registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $path) | ConvertFrom-Json
    } catch {
        Add-Problem Error "Registry is not valid JSON: $RelativePath - $($_.Exception.Message)"
        return $null
    }

    $context = "Registry $RelativePath"
    foreach ($field in @('type', 'status', 'canon_level', 'updated_real_date') + $ExtraTopFields) {
        Assert-RequiredTextField -Object $registry -Field $field -Context $context
    }

    if ((Get-PropertyValue -Object $registry -Name 'type') -ne $ExpectedType) {
        Add-Problem Error "$context has invalid type: $($registry.type)"
    }

    if ((Get-PropertyValue -Object $registry -Name 'status') -ne 'active') {
        Add-Problem Error "$context has invalid status: $($registry.status)"
    }

    if ((Get-PropertyValue -Object $registry -Name 'canon_level') -ne 'support') {
        Add-Problem Error "$context has invalid canon_level: $($registry.canon_level)"
    }

    $updatedDate = [string](Get-PropertyValue -Object $registry -Name 'updated_real_date')
    if (-not [string]::IsNullOrWhiteSpace($updatedDate) -and $updatedDate -notmatch '^\d{4}-\d{2}-\d{2}$') {
        Add-Problem Error "$context has invalid updated_real_date: $updatedDate"
    }

    Assert-GeneratedFrom -Registry $registry -Context $context
    return $registry
}

$validPriorities = @('критический', 'высокий', 'средний', 'низкий')

$decisionCount = 0
$questionCount = 0
$frontCount = 0
$pendingDecisionIds = @()
$acceptedDecisionIds = @()
$openQuestionIds = @()
$closedQuestionIds = @()
$questionHistoryIds = @()

$decisionRegistry = Read-RegistryJson -RelativePath '09_Реестры\Решения.json' -ExpectedType 'decision_registry'
if ($null -ne $decisionRegistry) {
    $decisions = Get-RegistryArray -Registry $decisionRegistry -Field 'decisions' -Context 'Decision registry'
    $decisionCount = $decisions.Count
    $decisionIds = @()

    foreach ($decision in $decisions) {
        $id = [string](Get-PropertyValue -Object $decision -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Decision registry entry' } else { "Decision registry entry $id" }

        foreach ($field in @(
            'id',
            'state',
            'real_date',
            'story_date',
            'player_character',
            'scene',
            'choice',
            'player_addition',
            'immediate_effect',
            'long_term_consequences',
            'links',
            'status_text'
        )) {
            Assert-RequiredTextField -Object $decision -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $decisionIds += $id
            if ($id -notmatch '^DEC(?:-PENDING)?-\d{3}$') {
                Add-Problem Error "$context has invalid ID format."
            }
        }

        Assert-AllowedValue -Object $decision -Field 'state' -Allowed @('accepted', 'pending') -Context $context
        $state = [string](Get-PropertyValue -Object $decision -Name 'state')

        if ($id -like 'DEC-PENDING-*' -and $state -ne 'pending') {
            Add-Problem Error "$context has DEC-PENDING-* ID but non-pending state: $state"
        }

        if ($id -match '^DEC-\d{3}$' -and $state -ne 'accepted') {
            Add-Problem Error "$context has DEC-* ID but non-accepted state: $state"
        }

        if ($state -eq 'pending') {
            foreach ($field in @('priority', 'question', 'owner', 'panel_status')) {
                Assert-RequiredTextField -Object $decision -Field $field -Context $context
            }

            Assert-AllowedValue -Object $decision -Field 'priority' -Allowed $validPriorities -Context $context
            Assert-AllowedValue -Object $decision -Field 'panel_status' -Allowed @('active', 'waiting', 'later') -Context $context
        }

        Assert-LinkText -Object $decision -Field 'links' -Context $context
    }

    Assert-UniqueValues -Values $decisionIds -Context 'Decision registry IDs'
    $pendingDecisionIds = Get-ObjectIds -Items @($decisions | Where-Object { $_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*' })
    $acceptedDecisionIds = Get-ObjectIds -Items @($decisions | Where-Object { $_.state -eq 'accepted' -and $_.id -match '^DEC-\d{3}$' })
}

$questionRegistry = Read-RegistryJson -RelativePath '09_Реестры\Вопросы.json' -ExpectedType 'question_registry' -ExtraTopFields @('current_chapter')
if ($null -ne $questionRegistry) {
    $questions = Get-RegistryArray -Registry $questionRegistry -Field 'questions' -Context 'Question registry'
    $history = Get-RegistryArray -Registry $questionRegistry -Field 'history' -Context 'Question registry' -AllowEmpty
    $questionCount = $questions.Count
    $questionIds = @()
    $resolvedQuestionIds = @()

    foreach ($question in $questions) {
        $id = [string](Get-PropertyValue -Object $question -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Question registry entry' } else { "Question registry entry $id" }

        foreach ($field in @('id', 'priority', 'text', 'owner', 'scope', 'status')) {
            Assert-RequiredTextField -Object $question -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $questionIds += $id
            if ($id -notmatch '^Q-(?:C\d+|WORLD)-\d{3}$') {
                Add-Problem Error "$context has invalid ID format."
            }
        }

        Assert-AllowedValue -Object $question -Field 'priority' -Allowed $validPriorities -Context $context
        Assert-AllowedValue -Object $question -Field 'status' -Allowed @('active', 'waiting', 'later', 'resolved') -Context $context
        Assert-AllowedValue -Object $question -Field 'scope' -Allowed @('chapter', 'world') -Context $context

        $scope = [string](Get-PropertyValue -Object $question -Name 'scope')
        $status = [string](Get-PropertyValue -Object $question -Name 'status')
        if ($id -match '^Q-C\d+-' -and $scope -ne 'chapter') {
            Add-Problem Error "$context has Q-C* ID but non-chapter scope: $scope"
        }

        if ($id -like 'Q-WORLD-*' -and $scope -ne 'world') {
            Add-Problem Error "$context has Q-WORLD-* ID but non-world scope: $scope"
        }

        if ($status -eq 'resolved') {
            $resolvedQuestionIds += $id
        }
    }

    Assert-UniqueValues -Values $questionIds -Context 'Question registry IDs'

    $historyIds = @()
    foreach ($historyItem in $history) {
        $id = [string](Get-PropertyValue -Object $historyItem -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Question registry history entry' } else { "Question registry history entry $id" }

        foreach ($field in @('date', 'id', 'resolution')) {
            Assert-RequiredTextField -Object $historyItem -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $historyIds += $id
            if ($id -notmatch '^Q-(?:C\d+|WORLD)-\d{3}$') {
                Add-Problem Error "$context has invalid ID format."
            }

            if ($resolvedQuestionIds -notcontains $id) {
                Add-Problem Error "$context references a question that is missing or not resolved."
            }
        }

        $date = [string](Get-PropertyValue -Object $historyItem -Name 'date')
        if (-not [string]::IsNullOrWhiteSpace($date) -and $date -notmatch '^\d{4}-\d{2}-\d{2}$') {
            Add-Problem Error "$context has invalid date: $date"
        }
    }

    Assert-UniqueValues -Values $historyIds -Context 'Question registry history IDs'
    foreach ($id in $resolvedQuestionIds) {
        if ($historyIds -notcontains $id) {
            Add-Problem Warning "Resolved question has no history entry in registry: $id"
        }
    }

    $openQuestionIds = Get-ObjectIds -Items @($questions | Where-Object { $_.status -ne 'resolved' })
    $closedQuestionIds = Get-ObjectIds -Items @($questions | Where-Object { $_.status -eq 'resolved' })
    $questionHistoryIds = @($historyIds)
}

if ($null -ne $decisionRegistry) {
    $pendingDecisionView = Read-RegistryJson -RelativePath '09_Реестры\Решения_незакрытые.json' -ExpectedType 'pending_decision_view'
    if ($null -ne $pendingDecisionView) {
        $viewDecisions = Get-RegistryArray -Registry $pendingDecisionView -Field 'decisions' -Context 'Pending decision view' -AllowEmpty
        Assert-SourceRegistry -View $pendingDecisionView -ExpectedSource '09_Реестры/Решения.json' -Context 'Pending decision view'
        Assert-ViewCount -View $pendingDecisionView -Items $viewDecisions -Context 'Pending decision view'
        Compare-IdSets 'pending decision view decisions' (Get-ObjectIds -Items $viewDecisions) 'decision registry pending decisions' $pendingDecisionIds

        foreach ($decision in $viewDecisions) {
            $id = [string](Get-PropertyValue -Object $decision -Name 'id')
            $state = [string](Get-PropertyValue -Object $decision -Name 'state')
            if ($id -notmatch '^DEC-PENDING-\d{3}$' -or $state -ne 'pending') {
                Add-Problem Error "Pending decision view contains non-pending decision: $id / $state"
            }
        }
    }

    $acceptedDecisionView = Read-RegistryJson -RelativePath '09_Реестры\Решения_закрытые.json' -ExpectedType 'accepted_decision_view'
    if ($null -ne $acceptedDecisionView) {
        $viewDecisions = Get-RegistryArray -Registry $acceptedDecisionView -Field 'decisions' -Context 'Accepted decision view' -AllowEmpty
        Assert-SourceRegistry -View $acceptedDecisionView -ExpectedSource '09_Реестры/Решения.json' -Context 'Accepted decision view'
        Assert-ViewCount -View $acceptedDecisionView -Items $viewDecisions -Context 'Accepted decision view'
        Compare-IdSets 'accepted decision view decisions' (Get-ObjectIds -Items $viewDecisions) 'decision registry accepted decisions' $acceptedDecisionIds

        foreach ($decision in $viewDecisions) {
            $id = [string](Get-PropertyValue -Object $decision -Name 'id')
            $state = [string](Get-PropertyValue -Object $decision -Name 'state')
            if ($id -notmatch '^DEC-\d{3}$' -or $state -ne 'accepted') {
                Add-Problem Error "Accepted decision view contains non-accepted decision: $id / $state"
            }
        }
    }
}

if ($null -ne $questionRegistry) {
    $openQuestionView = Read-RegistryJson -RelativePath '09_Реестры\Вопросы_открытые.json' -ExpectedType 'open_question_view' -ExtraTopFields @('current_chapter')
    if ($null -ne $openQuestionView) {
        $viewQuestions = Get-RegistryArray -Registry $openQuestionView -Field 'questions' -Context 'Open question view' -AllowEmpty
        Assert-SourceRegistry -View $openQuestionView -ExpectedSource '09_Реестры/Вопросы.json' -Context 'Open question view'
        Assert-ViewCount -View $openQuestionView -Items $viewQuestions -Context 'Open question view'
        Compare-IdSets 'open question view questions' (Get-ObjectIds -Items $viewQuestions) 'question registry open questions' $openQuestionIds

        foreach ($question in $viewQuestions) {
            if ([string](Get-PropertyValue -Object $question -Name 'status') -eq 'resolved') {
                Add-Problem Error "Open question view contains resolved question: $($question.id)"
            }
        }
    }

    $closedQuestionView = Read-RegistryJson -RelativePath '09_Реестры\Вопросы_закрытые.json' -ExpectedType 'closed_question_view' -ExtraTopFields @('current_chapter')
    if ($null -ne $closedQuestionView) {
        $viewQuestions = Get-RegistryArray -Registry $closedQuestionView -Field 'questions' -Context 'Closed question view' -AllowEmpty
        $viewHistory = Get-RegistryArray -Registry $closedQuestionView -Field 'history' -Context 'Closed question view' -AllowEmpty
        Assert-SourceRegistry -View $closedQuestionView -ExpectedSource '09_Реестры/Вопросы.json' -Context 'Closed question view'
        Assert-ViewCount -View $closedQuestionView -Items $viewQuestions -Context 'Closed question view'
        Compare-IdSets 'closed question view questions' (Get-ObjectIds -Items $viewQuestions) 'question registry resolved questions' $closedQuestionIds
        Compare-IdSets 'closed question view history' (Get-ObjectIds -Items $viewHistory) 'question registry history' $questionHistoryIds

        foreach ($question in $viewQuestions) {
            if ([string](Get-PropertyValue -Object $question -Name 'status') -ne 'resolved') {
                Add-Problem Error "Closed question view contains unresolved question: $($question.id)"
            }
        }
    }
}

$frontRegistry = Read-RegistryJson -RelativePath '09_Реестры\Фронты.json' -ExpectedType 'front_registry' -ExtraTopFields @('current_chapter', 'date_in_story')
if ($null -ne $frontRegistry) {
    $fronts = Get-RegistryArray -Registry $frontRegistry -Field 'fronts' -Context 'Front registry'
    $urgentForks = Get-RegistryArray -Registry $frontRegistry -Field 'urgent_forks' -Context 'Front registry'
    $activeFronts = Get-RegistryArray -Registry $frontRegistry -Field 'active_fronts' -Context 'Front registry'
    $timers = Get-RegistryArray -Registry $frontRegistry -Field 'timers' -Context 'Front registry'
    $frontCount = $fronts.Count
    $frontIds = @()

    foreach ($front in $fronts) {
        $id = [string](Get-PropertyValue -Object $front -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Front registry declaration' } else { "Front registry declaration $id" }
        foreach ($field in @('id', 'name')) {
            Assert-RequiredTextField -Object $front -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $frontIds += $id
            if ($id -notmatch '^FRONT-[A-Z0-9-]+$') {
                Add-Problem Error "$context has invalid FRONT-ID format."
            }
        }
    }

    Assert-UniqueValues -Values $frontIds -Context 'Front registry declarations'

    $urgentIds = @()
    foreach ($fork in $urgentForks) {
        $id = [string](Get-PropertyValue -Object $fork -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Front registry urgent fork' } else { "Front registry urgent fork $id" }
        foreach ($field in @('id', 'priority', 'front', 'summary', 'trigger', 'links')) {
            Assert-RequiredTextField -Object $fork -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $urgentIds += $id
            if ($id -notmatch '^FRONT-[A-Z0-9-]+$') {
                Add-Problem Error "$context has invalid FRONT-ID format."
            }

            if ($frontIds -notcontains $id) {
                Add-Problem Error "$context uses undeclared FRONT-ID."
            }
        }

        Assert-AllowedValue -Object $fork -Field 'priority' -Allowed $validPriorities -Context $context
        Assert-LinkArray -Object $fork -Field 'links' -Context $context
    }

    Assert-UniqueValues -Values $urgentIds -Context 'Front registry urgent forks'

    $activeIds = @()
    foreach ($activeFront in $activeFronts) {
        $id = [string](Get-PropertyValue -Object $activeFront -Name 'id')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Front registry active front' } else { "Front registry active front $id" }
        foreach ($field in @('id', 'front', 'participants', 'state', 'risk', 'next_trigger')) {
            Assert-RequiredTextField -Object $activeFront -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $activeIds += $id
            if ($id -notmatch '^FRONT-[A-Z0-9-]+$') {
                Add-Problem Error "$context has invalid FRONT-ID format."
            }

            if ($frontIds -notcontains $id) {
                Add-Problem Error "$context uses undeclared FRONT-ID."
            }
        }
    }

    Assert-UniqueValues -Values $activeIds -Context 'Front registry active fronts'

    $timerKeys = @()
    foreach ($timer in $timers) {
        $id = [string](Get-PropertyValue -Object $timer -Name 'id')
        $timerName = [string](Get-PropertyValue -Object $timer -Name 'timer')
        $context = if ([string]::IsNullOrWhiteSpace($id)) { 'Front registry timer' } else { "Front registry timer $id" }
        foreach ($field in @('id', 'timer', 'status', 'trigger')) {
            Assert-RequiredTextField -Object $timer -Field $field -Context $context
        }

        if (-not [string]::IsNullOrWhiteSpace($id)) {
            if ($id -notmatch '^FRONT-[A-Z0-9-]+$') {
                Add-Problem Error "$context has invalid FRONT-ID format."
            }

            if ($frontIds -notcontains $id) {
                Add-Problem Error "$context uses undeclared FRONT-ID."
            }

            if (-not [string]::IsNullOrWhiteSpace($timerName)) {
                $timerKeys += "$id|$timerName"
            }
        }
    }

    Assert-UniqueValues -Values $timerKeys -Context 'Front registry timer keys'
}

$summary = [pscustomobject]@{
    DecisionEntries = $decisionCount
    QuestionEntries = $questionCount
    FrontDeclarations = $frontCount
    Errors = $errors.Count
    Warnings = $warnings.Count
}

$result = [pscustomobject]@{
    Summary = $summary
    Errors = @($errors.ToArray())
    Warnings = @($warnings.ToArray())
}

if (-not $Quiet) {
    $summary | Format-List

    if ($warnings.Count -gt 0) {
        "`nWarnings:"
        $warnings | Sort-Object | ForEach-Object { "- $_" }
    }

    if ($errors.Count -gt 0) {
        "`nErrors:"
        $errors | Sort-Object | ForEach-Object { "- $_" }
    }
}

if ($PassThru) {
    $result
}

if ($errors.Count -gt 0 -and -not $PassThru) {
    exit 1
}

if ($errors.Count -eq 0 -and -not $Quiet) {
    "`nRegistry check completed successfully."
}
}
