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

function Test-ProjectPath {
    param(
        [string]$Reference,
        [string]$FromFile
    )

    if ($Reference -match '^[a-z]+://') {
        return $true
    }

    $normalized = $Reference -replace '/', '\'

    if ([System.IO.Path]::IsPathRooted($normalized)) {
        return Test-Path -LiteralPath $normalized
    }

    $rootCandidate = Join-Path $root $normalized
    if (Test-Path -LiteralPath $rootCandidate) {
        return $true
    }

    $fromDir = Split-Path -Parent $FromFile
    $relativeCandidate = Join-Path $fromDir $normalized
    return Test-Path -LiteralPath $relativeCandidate
}

function Resolve-ProjectPath {
    param(
        [string]$Reference,
        [string]$FromFile
    )

    $normalized = $Reference -replace '/', '\'

    if ([System.IO.Path]::IsPathRooted($normalized)) {
        if (Test-Path -LiteralPath $normalized) {
            return (Resolve-Path -LiteralPath $normalized).Path
        }

        return $null
    }

    $rootCandidate = Join-Path $root $normalized
    if (Test-Path -LiteralPath $rootCandidate) {
        return (Resolve-Path -LiteralPath $rootCandidate).Path
    }

    $fromDir = Split-Path -Parent $FromFile
    $relativeCandidate = Join-Path $fromDir $normalized
    if (Test-Path -LiteralPath $relativeCandidate) {
        return (Resolve-Path -LiteralPath $relativeCandidate).Path
    }

    return $null
}

function Get-ImageSize {
    param([string]$Path)

    try {
        Add-Type -AssemblyName System.Drawing
        $image = [System.Drawing.Image]::FromFile($Path)
        try {
            return [pscustomobject]@{
                Width = $image.Width
                Height = $image.Height
            }
        } finally {
            $image.Dispose()
        }
    } catch {
        return $null
    }
}

function Test-AcceptedPortraitRatio {
    param(
        [int]$Width,
        [int]$Height
    )

    if ($Width -le 0 -or $Height -le 0 -or $Width -ge $Height) {
        return $false
    }

    $ratio = $Width / $Height
    $isThreeByFour = [math]::Abs($ratio - 0.75) -le 0.04
    $isLegacyTwoByThree = [math]::Abs($ratio - (2 / 3)) -le 0.04
    return ($isThreeByFour -or $isLegacyTwoByThree)
}

function Test-PlannedReference {
    param(
        [string]$Reference,
        [string]$FromFile,
        [string]$Text
    )

    if ($Reference -notmatch '\.(jpg|jpeg|png|webp)$') {
        return $false
    }

    if ($Text -match '(?m)^portrait_status:\s*planned\s*$') {
        return $true
    }

    if ($Text -match '(?m)^status:\s*planned\s*$') {
        return $true
    }

    if ($Text -match '(?m)^type:\s*character_portrait_index\s*$') {
        foreach ($line in ($Text -split "\r?\n")) {
            if ($line.Contains($Reference) -and $line -match '\bplanned\b') {
                return $true
            }
        }
    }

    if ($Text -match '(?m)^type:\s*portrait_prompt\s*$' -and $Text -match '(?m)^status:\s*planned\s*$') {
        return $true
    }

    return $false
}

function Get-MetaType {
    param([string]$Text)

    if ($Text -match '(?s)^# .+?\r?\n\r?\n---\r?\n(.+?)\r?\n---') {
        $meta = $Matches[1]
        if ($meta -match '(?m)^type:\s*(.+?)\s*$') {
            return $Matches[1].Trim()
        }
    }

    return $null
}

function Get-MetaField {
    param(
        [string]$Text,
        [string]$Field
    )

    if ($Text -match '(?s)^# .+?\r?\n\r?\n---\r?\n(.+?)\r?\n---') {
        $meta = $Matches[1]
        $escapedField = [regex]::Escape($Field)
        if ($meta -match "(?m)^$escapedField\s*:\s*(.+?)\s*$") {
            return $Matches[1].Trim()
        }
    }

    return $null
}

function Get-SectionText {
    param(
        [string]$Text,
        [string]$Heading
    )

    $escapedHeading = [regex]::Escape($Heading)
    if ($Text -match "(?ms)^##\s+$escapedHeading\s*\r?\n(.+?)(?:\r?\n##\s+|\z)") {
        return $Matches[1]
    }

    return ''
}

function Convert-MarkdownTableRow {
    param([string]$Line)

    if ($Line -notmatch '^\|.+\|$' -or $Line -match '^\|\s*-') {
        return $null
    }

    return ,($Line.Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
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

$mdFiles = Get-ChildItem -LiteralPath $root -Recurse -Force -File -Filter '*.md' |
    Where-Object {
        $_.FullName -notmatch '\\.git\\' -and
        $_.FullName -notmatch '\\[^\\]*_MD_[^\\]*\\'
    }

$imageFiles = Get-ChildItem -LiteralPath $root -Recurse -Force -File |
    Where-Object {
        $_.FullName -notmatch '\\.git\\' -and
        $_.FullName -notmatch '\\[^\\]*_MD_[^\\]*\\' -and
        $_.Extension -match '^\.(jpg|jpeg|png|webp)$'
    }

$legacyScanExtensions = @('.md', '.ps1', '.json', '.yml', '.yaml', '.txt', '.gitignore', '.gitattributes', '.editorconfig', 'pre-commit')
$legacyScannerPaths = @(
    (Join-Path $root 'tools\Проверить_проект.ps1'),
    (Join-Path $root 'tools\Проверить_архив.ps1')
)

$textFilesForLegacyScan = Get-ChildItem -LiteralPath $root -Recurse -Force -File |
    Where-Object {
        $_.FullName -notmatch '\\.git\\' -and
        $_.FullName -notmatch '\\[^\\]*_MD_[^\\]*\\' -and
        $legacyScannerPaths -notcontains $_.FullName -and
        ($legacyScanExtensions -contains $_.Extension -or $legacyScanExtensions -contains $_.Name)
    }

$filesByType = @{}

foreach ($requiredPath in @(
    'AGENTS.md',
    '.githooks\pre-commit',
    '09_Реестры\Вопросы.json',
    '09_Реестры\Решения.json',
    '09_Реестры\Фронты.json',
    'tools\_lib.ps1',
    'tools\Завершить_ход.ps1',
    'tools\Закрыть_вопрос.ps1',
    'tools\Новый_портрет.ps1',
    'tools\Обновить_фронт.ps1',
    'tools\Проверить_архив.ps1',
    'tools\Проверить_инструменты.ps1',
    'tools\Проверить_реестры.ps1',
    'tools\Собрать_вопросы.ps1',
    'tools\Собрать_решения.ps1',
    'tools\Собрать_фронты.ps1',
    'tools\Собрать_индекс_источников.ps1',
    'tools\Собрать_индекс_сцен.ps1',
    'tools\Собрать_индекс_локаций.ps1',
    'tools\Собрать_индекс_персонажей.ps1',
    'tools\Собрать_панель_хода.ps1',
    'tools\Установить_git_hooks.ps1',
    'tools\Новый_персонаж.ps1',
    'tools\Новая_локация.ps1',
    'tools\Новая_сцена.ps1',
    'tools\Новый_вопрос.ps1',
    'tools\Новое_решение.ps1',
    'tools\Принять_сообщение.ps1',
    'tools\Обработать_входящее.ps1',
    'tools\Сцена_из_входящего.ps1',
    'tools\Новый_фронт.ps1',
    'tools\Закрыть_решение.ps1',
    'tools\Проверить_портреты.ps1',
    'tools\README.md',
    '08_Источники\00_Индекс_источников.md',
    '.github\workflows\project-check.yml'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $requiredPath))) {
        Add-Problem Error "Required support file is missing: $requiredPath"
    }
}

foreach ($forbiddenPath in @(
    '00_НАЧАТЬ_ОТСЮДА.md',
    '00_ТЕКУЩИЙ_КОНТЕКСТ_ДЛЯ_ИИ.md',
    '00_Словарь_имен_и_алиасов.md',
    '05_Текстовые_описания',
    '09_Шаблоны\Шаблон_текстового_описания.md',
    '10_ПРОСТОЙ_РЕЖИМ_ДЛЯ_ТЕБЯ.md',
    '11_Медиа\Иллюстрации_сцен',
    '11_Медиа\Карты_и_схемы',
    '11_Медиа\Портреты_персонажей\Правила_генерации_портретов.md',
    'AI_ПРОМПТ_ДЛЯ_ВЕДЕНИЯ_ИГРЫ.md',
    'AI_ПРОМПТ_ДЛЯ_ФОТО.md',
    'Инструкция_собрать_все_MD_в_одну_папку.md'
)) {
    if (Test-Path -LiteralPath (Join-Path $root $forbiddenPath)) {
        Add-Problem Error "Forbidden legacy path exists: $forbiddenPath"
    }
}

$gitignorePath = Join-Path $root '.gitignore'
if (-not (Test-Path -LiteralPath $gitignorePath)) {
    Add-Problem Warning '.gitignore file not found.'
} else {
    $gitignore = Get-Content -Raw -Encoding UTF8 -LiteralPath $gitignorePath
    if ($gitignore -notmatch '(?m)^.+_MD_.+/$') {
        Add-Problem Warning '.gitignore does not appear to ignore the temporary Markdown export folder.'
    }
}

foreach ($file in $mdFiles) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    $type = Get-MetaType -Text $text
    $relativeFile = Get-RelativePath $file.FullName
    $isTemplateFile = $relativeFile -like '09_*'

    foreach ($forbiddenReference in @(
        '00_НАЧАТЬ_ОТСЮДА.md',
        '00_ТЕКУЩИЙ_КОНТЕКСТ_ДЛЯ_ИИ.md',
        '05_Текстовые_описания',
        '09_Шаблоны/Шаблон_текстового_описания.md',
        '09_Шаблоны\Шаблон_текстового_описания.md',
        '10_ПРОСТОЙ_РЕЖИМ_ДЛЯ_ТЕБЯ.md',
        '11_Медиа/Иллюстрации_сцен',
        '11_Медиа\Иллюстрации_сцен',
        '11_Медиа/Карты_и_схемы',
        '11_Медиа\Карты_и_схемы',
        '11_Медиа/Портреты_персонажей/Правила_генерации_портретов.md',
        'AI_ПРОМПТ_ДЛЯ_ВЕДЕНИЯ_ИГРЫ.md',
        'AI_ПРОМПТ_ДЛЯ_ФОТО.md',
        'Инструкция_собрать_все_MD_в_одну_папку.md'
    )) {
        if ($text.Contains($forbiddenReference)) {
            Add-Problem Error "Forbidden legacy reference in $(Get-RelativePath $file.FullName): $forbiddenReference"
        }
    }

    if ($type -and -not $isTemplateFile) {
        if (-not $filesByType.ContainsKey($type)) {
            $filesByType[$type] = New-Object 'System.Collections.Generic.List[object]'
        }

        $filesByType[$type].Add($file) | Out-Null
    }

    $matches = [regex]::Matches($text, '`([^`]+\.(?:md|jpg|jpeg|png|webp))`')

    foreach ($match in $matches) {
        $ref = $match.Groups[1].Value
        if (
            -not (Test-ProjectPath -Reference $ref -FromFile $file.FullName) -and
            -not (Test-PlannedReference -Reference $ref -FromFile $file.FullName -Text $text)
        ) {
            Add-Problem Error "Broken local reference: $(Get-RelativePath $file.FullName) -> $ref"
        }
    }

    if (-not $isTemplateFile -and $text -match '(?s)^# .+?\r?\n\r?\n---\r?\n(.+?)\r?\n---') {
        $meta = $Matches[1]
        foreach ($field in @('type', 'status', 'canon_level')) {
            if ($meta -notmatch "(?m)^$field\s*:\s*\S+") {
                Add-Problem Warning "Missing front matter field '$field': $(Get-RelativePath $file.FullName)"
            }
        }
    }
}

foreach ($file in $textFilesForLegacyScan) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    foreach ($forbiddenReference in @(
        '00_НАЧАТЬ_ОТСЮДА.md',
        '00_ТЕКУЩИЙ_КОНТЕКСТ_ДЛЯ_ИИ.md',
        '05_Текстовые_описания',
        '09_Шаблоны/Шаблон_текстового_описания.md',
        '09_Шаблоны\Шаблон_текстового_описания.md',
        '10_ПРОСТОЙ_РЕЖИМ_ДЛЯ_ТЕБЯ.md',
        '11_Медиа/Иллюстрации_сцен',
        '11_Медиа\Иллюстрации_сцен',
        '11_Медиа/Карты_и_схемы',
        '11_Медиа\Карты_и_схемы',
        '11_Медиа/Портреты_персонажей/Правила_генерации_портретов.md',
        'AI_ПРОМПТ_ДЛЯ_ВЕДЕНИЯ_ИГРЫ.md',
        'AI_ПРОМПТ_ДЛЯ_ФОТО.md',
        'Инструкция_собрать_все_MD_в_одну_папку.md'
    )) {
        if ($text.Contains($forbiddenReference)) {
            Add-Problem Error "Forbidden legacy reference in text/service file $(Get-RelativePath $file.FullName): $forbiddenReference"
        }
    }
}

foreach ($script in Get-ChildItem -LiteralPath (Join-Path $root 'tools') -File -Filter '*.ps1') {
    $tokens = $null
    $parseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$parseErrors) | Out-Null
    foreach ($parseError in $parseErrors) {
        Add-Problem Error "PowerShell syntax error in $(Get-RelativePath $script.FullName): line $($parseError.Extent.StartLineNumber): $($parseError.Message)"
    }
}

$registryCheckPath = Join-Path $root 'tools\Проверить_реестры.ps1'
if (Test-Path -LiteralPath $registryCheckPath) {
    try {
        $registryCheck = & $registryCheckPath -Quiet -PassThru
        if ($null -eq $registryCheck) {
            Add-Problem Error 'Registry check returned no result.'
        } else {
            foreach ($warning in @($registryCheck.Warnings)) {
                if (-not [string]::IsNullOrWhiteSpace($warning)) {
                    Add-Problem Warning $warning
                }
            }

            foreach ($error in @($registryCheck.Errors)) {
                if (-not [string]::IsNullOrWhiteSpace($error)) {
                    Add-Problem Error $error
                }
            }
        }
    } catch {
        Add-Problem Error "Registry check failed to run: $($_.Exception.Message)"
    }
}

foreach ($requiredType in @(
    'ai_agent_instructions',
    'ai_current_context',
    'alias_index',
    'campaign_summary',
    'active_chapter',
    'decision_log',
    'open_questions',
    'closed_questions',
    'world_state',
    'front_tracker',
    'source_index',
    'location_index',
    'scene_index',
    'next_turn_panel',
    'tool_index',
    'character_index',
    'character_portrait_index'
)) {
    if (-not $filesByType.ContainsKey($requiredType)) {
        Add-Problem Warning "Required support type not found: $requiredType"
    }
}

if (-not $filesByType.ContainsKey('location_index')) {
    Add-Problem Error 'location_index file not found.'
} else {
    $locationIndexPath = $filesByType['location_index'][0].FullName
    $locationIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $locationIndexPath
    if ($locationIndex -notmatch '(?m)^generated_by:\s*tools/Собрать_индекс_локаций\.ps1\s*$') {
        Add-Problem Warning "Location index should be generated by tools/Собрать_индекс_локаций.ps1: $(Get-RelativePath $locationIndexPath)"
    }

    $locationRefs = [regex]::Matches($locationIndex, '`(04_Локации/[^`]+\.md)`') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    foreach ($ref in $locationRefs) {
        if (-not (Test-ProjectPath -Reference $ref -FromFile $locationIndexPath)) {
            Add-Problem Error "Location index points to missing file: $ref"
        }
    }

    if ($filesByType.ContainsKey('location')) {
        $locationFiles = $filesByType['location'] |
            ForEach-Object { (Get-RelativePath $_.FullName) -replace '\\', '/' }

        foreach ($file in $locationFiles) {
            if ($locationRefs -notcontains $file) {
                Add-Problem Error "Location card is missing from location index: $file"
            }
        }
    }
}

if ($filesByType.ContainsKey('next_turn_panel')) {
    $nextTurnPath = $filesByType['next_turn_panel'][0].FullName
    $nextTurn = Get-Content -Raw -Encoding UTF8 -LiteralPath $nextTurnPath
    if ($nextTurn -notmatch '(?m)^generated_by:\s*tools/Собрать_панель_хода\.ps1\s*$') {
        Add-Problem Warning "Next turn panel should be generated by tools/Собрать_панель_хода.ps1: $(Get-RelativePath $nextTurnPath)"
    }
}

if ($filesByType.ContainsKey('scene_index')) {
    $sceneIndexPath = $filesByType['scene_index'][0].FullName
    $sceneIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $sceneIndexPath
    if ($sceneIndex -notmatch '(?m)^generated_by:\s*tools/Собрать_индекс_сцен\.ps1\s*$') {
        Add-Problem Warning "Scene index should be generated by tools/Собрать_индекс_сцен.ps1: $(Get-RelativePath $sceneIndexPath)"
    }

    $sceneRefs = [regex]::Matches($sceneIndex, '`(01_Кампания/Ветки/[^`]+\.md)`') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    foreach ($ref in $sceneRefs) {
        if (-not (Test-ProjectPath -Reference $ref -FromFile $sceneIndexPath)) {
            Add-Problem Error "Scene index points to missing file: $ref"
        }
    }

    if ($filesByType.ContainsKey('scene')) {
        $sceneFiles = $filesByType['scene'] |
            ForEach-Object { (Get-RelativePath $_.FullName) -replace '\\', '/' }

        foreach ($file in $sceneFiles) {
            if ($sceneRefs -notcontains $file) {
                Add-Problem Error "Scene file is missing from scene index: $file"
            }
        }
    }
}

if (-not $filesByType.ContainsKey('source_index')) {
    Add-Problem Error 'source_index file not found.'
} else {
    $sourceIndexPath = $filesByType['source_index'][0].FullName
    $sourceIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $sourceIndexPath
    if ($sourceIndex -notmatch '(?m)^generated_by:\s*tools/Собрать_индекс_источников\.ps1\s*$') {
        Add-Problem Warning "Source index should be generated by tools/Собрать_индекс_источников.ps1: $(Get-RelativePath $sourceIndexPath)"
    }

    $sourceRefs = [regex]::Matches($sourceIndex, '`(08_Источники/[^`]+\.md)`') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    foreach ($ref in $sourceRefs) {
        if (-not (Test-ProjectPath -Reference $ref -FromFile $sourceIndexPath)) {
            Add-Problem Error "Source index points to missing file: $ref"
        }
    }

    $sourceFiles = Get-ChildItem -LiteralPath (Join-Path $root '08_Источники') -File -Filter '*.md' |
        Where-Object { $_.Name -ne '00_Индекс_источников.md' } |
        ForEach-Object { (Get-RelativePath $_.FullName) -replace '\\', '/' }

    foreach ($file in $sourceFiles) {
        if ($sourceRefs -notcontains $file) {
            Add-Problem Error "Source file is missing from source index: $file"
        }
    }
}

if (-not $filesByType.ContainsKey('character_index')) {
    Add-Problem Error 'character_index file not found.'
} else {
    $characterIndexPath = $filesByType['character_index'][0].FullName
    $characterIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $characterIndexPath
    if ($characterIndex -notmatch '(?m)^generated_by:\s*tools/Собрать_индекс_персонажей\.ps1\s*$') {
        Add-Problem Warning "Character index should be generated by tools/Собрать_индекс_персонажей.ps1: $(Get-RelativePath $characterIndexPath)"
    }

    $characterRefs = [regex]::Matches($characterIndex, '`([^`]+\.md)`') |
        ForEach-Object { $_.Groups[1].Value } |
        Where-Object { $_ -like '03_*' }

    $characterFiles = @()
    if ($filesByType.ContainsKey('character')) {
        $characterFiles = $filesByType['character'] |
            ForEach-Object { (Get-RelativePath $_.FullName) -replace '\\', '/' }
    }

    foreach ($ref in $characterRefs) {
        if (-not (Test-ProjectPath -Reference $ref -FromFile $characterIndexPath)) {
            Add-Problem Error "Character index points to missing file: $ref"
        }
    }

    foreach ($file in $characterFiles) {
        if ($characterRefs -notcontains $file) {
            Add-Problem Error "Character card is missing from index: $file"
        }
    }

    $characterIndexRows = [regex]::Matches(
        $characterIndex,
        '(?m)^\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*`([^`]+\.md)`\s*\|\s*$'
    )

    foreach ($row in $characterIndexRows) {
        $characterName = $row.Groups[1].Value.Trim()
        $indexPortraitStatus = $row.Groups[3].Value.Trim()
        $characterRef = $row.Groups[4].Value.Trim()

        if ($characterRef -notlike '03_*') {
            continue
        }

        $normalizedRef = $characterRef -replace '/', '\'
        $characterPath = Join-Path $root $normalizedRef

        if (-not (Test-Path -LiteralPath $characterPath)) {
            $characterPath = Join-Path (Split-Path -Parent $characterIndexPath) $normalizedRef
        }

        if (-not (Test-Path -LiteralPath $characterPath)) {
            continue
        }

        $characterText = Get-Content -Raw -Encoding UTF8 -LiteralPath $characterPath

        if ($characterText -notmatch '(?m)^portrait_status:\s*(available|missing|planned)\s*$') {
            continue
        }

        $portraitStatus = $Matches[1].Trim()
        $indexStatusAvailable = -join [char[]](0x0435, 0x0441, 0x0442, 0x044C)
        $indexStatusMissing = -join [char[]](0x043D, 0x0443, 0x0436, 0x0435, 0x043D)
        $indexStatusPlanned = -join [char[]](0x0437, 0x0430, 0x043F, 0x043B, 0x0430, 0x043D, 0x0438, 0x0440, 0x043E, 0x0432, 0x0430, 0x043D)
        $expectedIndexStatus = switch ($portraitStatus) {
            'available' { $indexStatusAvailable }
            'missing' { $indexStatusMissing }
            'planned' { $indexStatusPlanned }
        }

        if ($indexPortraitStatus -ne $expectedIndexStatus) {
            Add-Problem Warning "Character index portrait status mismatch: $characterName uses '$indexPortraitStatus' but card expects '$expectedIndexStatus': $characterRef"
        }
    }
}

if ($filesByType.ContainsKey('character')) {
    foreach ($file in $filesByType['character']) {
        $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName

        if ($text -notmatch '(?m)^portrait:\s*(.+)\s*$') {
            Add-Problem Error "Character card has no portrait field: $(Get-RelativePath $file.FullName)"
            continue
        }

        $portrait = $Matches[1].Trim()

        if ($text -notmatch '(?m)^portrait_status:\s*(available|missing|planned)\s*$') {
            Add-Problem Error "Character card has invalid portrait_status: $(Get-RelativePath $file.FullName)"
            continue
        }

        $portraitStatus = $Matches[1].Trim()

        if ($portraitStatus -eq 'available') {
            $portraitFullPath = Resolve-ProjectPath -Reference $portrait -FromFile $file.FullName
            if ($portrait -eq 'null' -or -not $portraitFullPath) {
                Add-Problem Error "portrait_status=available but portrait file is missing: $(Get-RelativePath $file.FullName)"
            } else {
                $size = Get-ImageSize -Path $portraitFullPath
                if (-not $size) {
                    Add-Problem Warning "Cannot read portrait dimensions: $(Get-RelativePath $portraitFullPath)"
                } elseif (-not (Test-AcceptedPortraitRatio -Width $size.Width -Height $size.Height)) {
                    Add-Problem Error "Portrait has invalid aspect ratio, expected 3:4 or legacy 2:3: $(Get-RelativePath $portraitFullPath) ($($size.Width)x$($size.Height))"
                }
            }
        }
    }
}

if ($filesByType.ContainsKey('portrait_prompt')) {
    foreach ($file in $filesByType['portrait_prompt']) {
        $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName

        if ($text -notmatch '(?m)^character:\s*\S+') {
            Add-Problem Warning "Portrait prompt has no character field: $(Get-RelativePath $file.FullName)"
        }

        if ($text -notmatch '(?m)^(output|result):\s*11_Медиа/Портреты_персонажей/.+\.(jpg|jpeg|png|webp)\s*$') {
            Add-Problem Warning "Portrait prompt has no output/result image path: $(Get-RelativePath $file.FullName)"
        }
    }
}

if (-not $filesByType.ContainsKey('character_portrait_index')) {
    Add-Problem Error 'character_portrait_index file not found.'
} else {
    $portraitIndexPath = $filesByType['character_portrait_index'][0].FullName
    $portraitIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $portraitIndexPath
    $portraitRoot = Split-Path -Parent $portraitIndexPath
    $portraitRefs = [regex]::Matches($portraitIndex, '`([^`]+\.(?:jpg|jpeg|png|webp))`') |
        ForEach-Object { $_.Groups[1].Value }
    $portraitImageFiles = Get-ChildItem -LiteralPath $portraitRoot -Recurse -File |
        Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|webp)$' } |
        ForEach-Object { (Get-RelativePath $_.FullName) -replace '\\', '/' }

    foreach ($ref in $portraitRefs) {
        if (
            -not (Test-ProjectPath -Reference $ref -FromFile $portraitIndexPath) -and
            -not (Test-PlannedReference -Reference $ref -FromFile $portraitIndexPath -Text $portraitIndex)
        ) {
            Add-Problem Error "Portrait index points to missing file: $ref"
        }
    }

    foreach ($file in $portraitImageFiles) {
        if ($portraitRefs -notcontains $file) {
            Add-Problem Error "Portrait image is missing from portrait index: $file"
        }
    }

    foreach ($dir in Get-ChildItem -LiteralPath $portraitRoot -Directory) {
        $hasImage = (Get-ChildItem -LiteralPath $dir.FullName -File |
            Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|webp)$' } |
            Measure-Object).Count -gt 0
        $hasPrompt = $false

        foreach ($promptFile in Get-ChildItem -LiteralPath $dir.FullName -File -Filter '*.md') {
            $promptText = Get-Content -Raw -Encoding UTF8 -LiteralPath $promptFile.FullName
            if ((Get-MetaType -Text $promptText) -eq 'portrait_prompt') {
                $hasPrompt = $true
                break
            }
        }

        if ($hasImage -and -not $hasPrompt) {
            Add-Problem Warning "Portrait folder has image but no portrait_prompt file: $(Get-RelativePath $dir.FullName)"
        }
    }
}

$decisionIds = @()

if ($filesByType.ContainsKey('decision_log')) {
    $decisionLogPath = $filesByType['decision_log'][0].FullName
    $decisionLog = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
    if ($decisionLog -notmatch '(?m)^source_registry:\s*09_Реестры/Решения\.json\s*$') {
        Add-Problem Warning "decision_log should be generated from 09_Реестры/Решения.json: $(Get-RelativePath $decisionLogPath)"
    }

    $decisionIds = [regex]::Matches($decisionLog, '(?m)^###\s+(DEC(?:-PENDING)?-\d{3})\s*$') |
        ForEach-Object { $_.Groups[1].Value }

    if ($decisionIds.Count -eq 0) {
        Add-Problem Warning 'No decision IDs found in decision_log headings.'
    }

    $duplicateDecisionIds = $decisionIds |
        Group-Object |
        Where-Object { $_.Count -gt 1 } |
        ForEach-Object { $_.Name }

    foreach ($id in $duplicateDecisionIds) {
        Add-Problem Error "Duplicate decision ID in decision log: $id"
    }

    $acceptedDecisionIds = [regex]::Matches($decisionLog, '(?m)^###\s+DEC-(\d{3})\s*$') |
        ForEach-Object {
            [pscustomobject]@{
                Id = [int]$_.Groups[1].Value
                Text = $_.Value.Trim()
            }
        }

    for ($i = 1; $i -lt $acceptedDecisionIds.Count; $i++) {
        if ($acceptedDecisionIds[$i].Id -lt $acceptedDecisionIds[$i - 1].Id) {
            Add-Problem Warning "Decision IDs are out of order: $($acceptedDecisionIds[$i].Text) follows $($acceptedDecisionIds[$i - 1].Text)"
        }
    }
}

$activePendingDecisionIds = @()
$openQuestionIds = @()
$closedQuestionIds = @()

if ($filesByType.ContainsKey('open_questions')) {
    $openQuestionsPath = $filesByType['open_questions'][0].FullName
    $openQuestions = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
    if ($openQuestions -notmatch '(?m)^source_registry:\s*09_Реестры/Вопросы\.json\s*$') {
        Add-Problem Warning "open_questions should be generated from 09_Реестры/Вопросы.json: $(Get-RelativePath $openQuestionsPath)"
    }

    $questionIds = @(
        [regex]::Matches($openQuestions, '(?m)^\|\s*(Q-(?:C2|WORLD)-\d{3})\s*\|') |
            ForEach-Object { $_.Groups[1].Value }
    )
    $openQuestionIds = $questionIds

    $openQuestionRowsWithIds = [regex]::Matches($openQuestions, '(?m)^\|\s*((?:DEC-PENDING|Q-(?:C2|WORLD))-\d{3})\s*\|') |
        ForEach-Object { $_.Value }

    if ($questionIds.Count -eq 0) {
        Add-Problem Warning 'No Q-C2/Q-WORLD IDs found in open_questions.'
    }

    $duplicateQuestionIds = $questionIds |
        Group-Object |
        Where-Object { $_.Count -gt 1 } |
        ForEach-Object { $_.Name }

    foreach ($id in $duplicateQuestionIds) {
        Add-Problem Error "Duplicate open question ID: $id"
    }

    $validQuestionStatuses = @('active', 'waiting', 'later')
    $questionRowsWithStatus = [regex]::Matches(
        $openQuestions,
        '(?m)^\|\s*((?:DEC-PENDING|Q-(?:C2|WORLD))-\d{3})\s*\|\s*[^|]+\|\s*[^|]+\|\s*[^|]+\|\s*([^|]+?)\s*\|\s*$'
    )

    if ($questionRowsWithStatus.Count -ne $openQuestionRowsWithIds.Count) {
        Add-Problem Error 'Some rows in open_questions have no status column.'
    }

    foreach ($row in $questionRowsWithStatus) {
        $id = $row.Groups[1].Value.Trim()
        $questionStatus = $row.Groups[2].Value.Trim()

        if ($id -match '^Q-(?:C2|WORLD)-\d{3}$' -and $questionStatus -eq 'resolved') {
            Add-Problem Error "Resolved question $id must be moved to closed_questions."
            continue
        }

        if ($validQuestionStatuses -notcontains $questionStatus) {
            Add-Problem Error "Invalid open question status for ${id}: $questionStatus"
        }
    }

    $pendingQuestionIds = @(
        $questionRowsWithStatus |
            Where-Object {
                $_.Groups[1].Value.Trim() -match '^DEC-PENDING-\d{3}$' -and
                $_.Groups[2].Value.Trim() -ne 'resolved'
            } |
            ForEach-Object { $_.Groups[1].Value.Trim() }
    )
    $activePendingDecisionIds = $pendingQuestionIds

    if ($filesByType.ContainsKey('decision_log')) {
        $decisionLogPath = $filesByType['decision_log'][0].FullName
        $decisionLog = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
        $pendingDecisionIds = [regex]::Matches($decisionLog, '(?m)^###\s+(DEC-PENDING-\d{3})\s*$') |
            ForEach-Object { $_.Groups[1].Value }

        Compare-IdSets 'decision_log pending decisions' $pendingDecisionIds 'open_questions pending decisions' $pendingQuestionIds
    }

    if ($filesByType.ContainsKey('ai_current_context')) {
        $currentContextPath = $filesByType['ai_current_context'][0].FullName
        $currentContext = Get-Content -Raw -Encoding UTF8 -LiteralPath $currentContextPath
        $contextPendingIds = [regex]::Matches($currentContext, '\bDEC-PENDING-\d{3}\b') |
            ForEach-Object { $_.Value }

        Compare-IdSets 'current context pending decisions' $contextPendingIds 'open_questions pending decisions' $pendingQuestionIds
    }
}

$decisionRegistryPath = Join-Path $root '09_Реестры\Решения.json'
if (Test-Path -LiteralPath $decisionRegistryPath) {
    $decisionRegistry = $null
    try {
        $decisionRegistry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionRegistryPath) | ConvertFrom-Json
    } catch {
        Add-Problem Error "Decision registry is not valid JSON: 09_Реестры\Решения.json - $($_.Exception.Message)"
    }

    if ($null -ne $decisionRegistry) {
        if ($decisionRegistry.type -ne 'decision_registry') {
            Add-Problem Error "Decision registry has invalid type: $($decisionRegistry.type)"
        }

        $registryDecisions = @($decisionRegistry.decisions)
        if ($registryDecisions.Count -eq 0) {
            Add-Problem Error 'Decision registry contains no decisions.'
        }

        $registryDecisionIds = @(
            $registryDecisions |
                ForEach-Object { $_.id }
        )

        $duplicateRegistryDecisionIds = $registryDecisionIds |
            Group-Object |
            Where-Object { $_.Count -gt 1 } |
            ForEach-Object { $_.Name }

        foreach ($id in $duplicateRegistryDecisionIds) {
            Add-Problem Error "Duplicate decision ID in registry: $id"
        }

        foreach ($decision in $registryDecisions) {
            if ($decision.id -notmatch '^DEC(?:-PENDING)?-\d{3}$') {
                Add-Problem Error "Invalid decision ID in registry: $($decision.id)"
            }

            if ($decision.state -notin @('pending', 'accepted')) {
                Add-Problem Error "Invalid decision state in registry for $($decision.id): $($decision.state)"
            }

            if ($decision.id -like 'DEC-PENDING-*' -and $decision.state -ne 'pending') {
                Add-Problem Error "Pending decision has non-pending state in registry: $($decision.id)"
            }

            if ($decision.id -match '^DEC-\d{3}$' -and $decision.state -ne 'accepted') {
                Add-Problem Error "Accepted decision has non-accepted state in registry: $($decision.id)"
            }

            if ($decision.state -eq 'pending') {
                foreach ($field in @('priority', 'question', 'owner', 'panel_status')) {
                    if ([string]::IsNullOrWhiteSpace($decision.$field)) {
                        Add-Problem Error "Pending decision $($decision.id) has empty registry field: $field"
                    }
                }
            }
        }

        $registryPendingDecisionIds = @(
            $registryDecisions |
                Where-Object { $_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*' } |
                ForEach-Object { $_.id }
        )

        Compare-IdSets 'decision registry decisions' $registryDecisionIds 'decision_log headings' $decisionIds
        Compare-IdSets 'decision registry pending decisions' $registryPendingDecisionIds 'open_questions pending decisions' $activePendingDecisionIds
    }
}

if ($filesByType.ContainsKey('closed_questions')) {
    $closedQuestionsPath = $filesByType['closed_questions'][0].FullName
    $closedQuestions = Get-Content -Raw -Encoding UTF8 -LiteralPath $closedQuestionsPath
    if ($closedQuestions -notmatch '(?m)^source_registry:\s*09_Реестры/Вопросы\.json\s*$') {
        Add-Problem Warning "closed_questions should be generated from 09_Реестры/Вопросы.json: $(Get-RelativePath $closedQuestionsPath)"
    }

    $closedQuestionRowsWithStatus = [regex]::Matches(
        $closedQuestions,
        '(?m)^\|\s*(Q-(?:C2|WORLD)-\d{3})\s*\|\s*[^|]+\|\s*[^|]+\|\s*[^|]+\|\s*([^|]+?)\s*\|\s*$'
    )
    $closedQuestionIds = @(
        $closedQuestionRowsWithStatus |
            ForEach-Object { $_.Groups[1].Value.Trim() }
    )

    if ($closedQuestionIds.Count -eq 0) {
        Add-Problem Warning 'No Q-C2/Q-WORLD IDs found in closed_questions.'
    }

    $duplicateClosedQuestionIds = $closedQuestionIds |
        Group-Object |
        Where-Object { $_.Count -gt 1 } |
        ForEach-Object { $_.Name }

    foreach ($id in $duplicateClosedQuestionIds) {
        Add-Problem Error "Duplicate closed question ID: $id"
    }

    foreach ($row in $closedQuestionRowsWithStatus) {
        $id = $row.Groups[1].Value.Trim()
        $questionStatus = $row.Groups[2].Value.Trim()
        if ($questionStatus -ne 'resolved') {
            Add-Problem Error "Invalid closed question status for ${id}: $questionStatus"
        }
    }

    foreach ($id in ($closedQuestionIds | Sort-Object -Unique)) {
        if ($openQuestionIds -contains $id) {
            Add-Problem Error "Question ID exists in both open_questions and closed_questions: $id"
        }
    }

    if ($closedQuestions.Contains('$QuestionId')) {
        Add-Problem Error 'closed_questions contains an unexpanded $QuestionId marker.'
    }

    if ($closedQuestions.Contains('resolved${')) {
        Add-Problem Error 'closed_questions contains a malformed resolved replacement marker.'
    }
}

$questionRegistryPath = Join-Path $root '09_Реестры\Вопросы.json'
if (Test-Path -LiteralPath $questionRegistryPath) {
    $questionRegistry = $null
    try {
        $questionRegistry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath) | ConvertFrom-Json
    } catch {
        Add-Problem Error "Question registry is not valid JSON: 09_Реестры\Вопросы.json - $($_.Exception.Message)"
    }

    if ($null -ne $questionRegistry) {
        if ($questionRegistry.type -ne 'question_registry') {
            Add-Problem Error "Question registry has invalid type: $($questionRegistry.type)"
        }

        $registryQuestions = @($questionRegistry.questions)
        if ($registryQuestions.Count -eq 0) {
            Add-Problem Error 'Question registry contains no questions.'
        }

        $registryIds = @(
            $registryQuestions |
                ForEach-Object { $_.id }
        )

        $duplicateRegistryIds = $registryIds |
            Group-Object |
            Where-Object { $_.Count -gt 1 } |
            ForEach-Object { $_.Name }

        foreach ($id in $duplicateRegistryIds) {
            Add-Problem Error "Duplicate question ID in registry: $id"
        }

        $validRegistryStatuses = @('active', 'waiting', 'later', 'resolved')
        foreach ($question in $registryQuestions) {
            if ($question.id -notmatch '^Q-(?:C2|WORLD)-\d{3}$') {
                Add-Problem Error "Invalid question ID in registry: $($question.id)"
            }

            if ($validRegistryStatuses -notcontains $question.status) {
                Add-Problem Error "Invalid question status in registry for $($question.id): $($question.status)"
            }

            if ($question.scope -notin @('chapter', 'world')) {
                Add-Problem Error "Invalid question scope in registry for $($question.id): $($question.scope)"
            }

            if ($question.id -like 'Q-C2-*' -and $question.scope -ne 'chapter') {
                Add-Problem Error "Chapter question has non-chapter scope in registry: $($question.id)"
            }

            if ($question.id -like 'Q-WORLD-*' -and $question.scope -ne 'world') {
                Add-Problem Error "World question has non-world scope in registry: $($question.id)"
            }
        }

        $registryOpenQuestionIds = @(
            $registryQuestions |
                Where-Object { $_.status -ne 'resolved' } |
                ForEach-Object { $_.id }
        )
        $registryClosedQuestionIds = @(
            $registryQuestions |
                Where-Object { $_.status -eq 'resolved' } |
                ForEach-Object { $_.id }
        )

        Compare-IdSets 'question registry open questions' $registryOpenQuestionIds 'open_questions question rows' $openQuestionIds
        Compare-IdSets 'question registry resolved questions' $registryClosedQuestionIds 'closed_questions question rows' $closedQuestionIds

        $historyItems = @($questionRegistry.history)
        $historyIds = @($historyItems | ForEach-Object { $_.id })
        foreach ($historyId in ($historyIds | Sort-Object -Unique)) {
            if ($registryClosedQuestionIds -notcontains $historyId) {
                Add-Problem Error "Question registry history references a missing or unresolved question: $historyId"
            }
        }

        foreach ($id in ($registryClosedQuestionIds | Sort-Object -Unique)) {
            if ($historyIds -notcontains $id) {
                Add-Problem Warning "Resolved question has no history entry in registry: $id"
            }
        }
    }
}

$liveContextFilesByPath = @{}
function Add-LiveContextFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return
    }

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $liveContextFilesByPath[$resolved] = $true
}

foreach ($typeName in @(
    'ai_current_context',
    'campaign_summary',
    'active_chapter',
    'open_questions',
    'front_tracker',
    'next_turn_panel'
)) {
    if ($filesByType.ContainsKey($typeName)) {
        foreach ($file in $filesByType[$typeName]) {
            Add-LiveContextFile -Path $file.FullName
        }
    }
}

if ($filesByType.ContainsKey('chapter')) {
    foreach ($file in $filesByType['chapter']) {
        $chapterText = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        if ((Get-MetaField -Text $chapterText -Field 'status') -eq 'active') {
            Add-LiveContextFile -Path $file.FullName
        }
    }
}

if ($filesByType.ContainsKey('character_branch')) {
    foreach ($file in $filesByType['character_branch']) {
        Add-LiveContextFile -Path $file.FullName
    }
}

$liveContextFiles = @(
    $liveContextFilesByPath.Keys |
        ForEach-Object { Get-Item -LiteralPath $_ } |
        Sort-Object FullName
)

foreach ($file in $liveContextFiles) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    $relativeFile = Get-RelativePath $file.FullName
    $pendingRefs = @(
        [regex]::Matches($text, '\bDEC-PENDING-\d{3}\b') |
            ForEach-Object { $_.Value } |
            Sort-Object -Unique
    )

    foreach ($id in $pendingRefs) {
        if ($activePendingDecisionIds -notcontains $id) {
            Add-Problem Error "Stale pending decision reference in live context: $relativeFile -> $id"
        }
    }
}

$sceneStatusByRef = @{}
if ($filesByType.ContainsKey('scene')) {
    foreach ($file in $filesByType['scene']) {
        $sceneText = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        $sceneRef = (Get-RelativePath $file.FullName) -replace '\\', '/'
        $sceneStatusByRef[$sceneRef] = Get-MetaField -Text $sceneText -Field 'status'
    }
}

foreach ($file in $liveContextFiles) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    $relativeFile = Get-RelativePath $file.FullName
    $activeSceneSection = Get-SectionText -Text $text -Heading 'Активные сцены'
    $refsInActiveSceneSection = @(
        [regex]::Matches($activeSceneSection, '`(01_Кампания/Ветки/[^`]+\.md)`') |
            ForEach-Object { $_.Groups[1].Value }
    )

    foreach ($ref in $refsInActiveSceneSection) {
        if ($sceneStatusByRef.ContainsKey($ref) -and $sceneStatusByRef[$ref] -ne 'active') {
            Add-Problem Error "Closed scene is listed in active scenes: $relativeFile -> $ref has status '$($sceneStatusByRef[$ref])'"
        }
    }

    foreach ($line in ($text -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -eq $cells) {
            continue
        }

        $activeStatusCell = @($cells | Where-Object { $_.ToLowerInvariant() -match '^(active|актив)' }).Count -gt 0
        if (-not $activeStatusCell) {
            continue
        }

        $sceneRefs = @(
            [regex]::Matches($line, '`(01_Кампания/Ветки/[^`]+\.md)`') |
                ForEach-Object { $_.Groups[1].Value }
        )

        foreach ($ref in $sceneRefs) {
            if ($sceneStatusByRef.ContainsKey($ref) -and $sceneStatusByRef[$ref] -ne 'active') {
                Add-Problem Error "Scene table row is marked active but scene is not active: $relativeFile -> $ref has status '$($sceneStatusByRef[$ref])'"
            }
        }
    }
}

$staleActiveMarkers = @(
    [pscustomobject]@{
        Name = 'Hector liner destination'
        Pattern = 'куда\s+отправить\s+захваченн\w*\s+линкор|захваченн\w*\s+линкор.{0,120}Бел\w*\s+Гаван\w*.{0,120}Порт-Вингард|линкор.{0,80}готов.{0,80}отправк\w*.{0,80}изучен'
        Message = 'The Hector liner destination is already resolved: the captured liner is in Port-Wingard.'
    }
)

foreach ($file in $liveContextFiles) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    $relativeFile = Get-RelativePath $file.FullName

    foreach ($marker in $staleActiveMarkers) {
        if ([regex]::IsMatch($text, $marker.Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
            Add-Problem Error "Stale active branch marker in ${relativeFile}: $($marker.Name). $($marker.Message)"
        }
    }
}

$declaredFrontIdSet = @()

if ($filesByType.ContainsKey('front_tracker')) {
    $frontTrackerPath = $filesByType['front_tracker'][0].FullName
    $frontTracker = Get-Content -Raw -Encoding UTF8 -LiteralPath $frontTrackerPath
    if ($frontTracker -notmatch '(?m)^source_registry:\s*09_Реестры/Фронты\.json\s*$') {
        Add-Problem Warning "front_tracker should be generated from 09_Реестры/Фронты.json: $(Get-RelativePath $frontTrackerPath)"
    }

    $frontIdSection = Get-SectionText -Text $frontTracker -Heading 'Справочник FRONT-ID'
    $declaredFrontIds = [regex]::Matches($frontIdSection, '(?m)^\|\s*(FRONT-[A-Z0-9-]+)\s*\|') |
        ForEach-Object { $_.Groups[1].Value }

    if ($declaredFrontIds.Count -eq 0) {
        Add-Problem Error 'No FRONT-ID declarations found in front tracker.'
    }

    $duplicateFrontIds = $declaredFrontIds |
        Group-Object |
        Where-Object { $_.Count -gt 1 } |
        ForEach-Object { $_.Name }

    foreach ($id in $duplicateFrontIds) {
        Add-Problem Error "Duplicate FRONT-ID declaration: $id"
    }

    $declaredFrontIdSet = @($declaredFrontIds | Sort-Object -Unique)
    $usedFrontIds = [regex]::Matches($frontTracker, '\bFRONT-[A-Z0-9-]+\b') |
        ForEach-Object { $_.Value } |
        Where-Object { $_ -ne 'FRONT-ID' } |
        Sort-Object -Unique

    foreach ($id in $usedFrontIds) {
        if ($declaredFrontIdSet -notcontains $id) {
            Add-Problem Error "FRONT-ID is used but not declared in front tracker: $id"
        }
    }

    $activeFrontSection = Get-SectionText -Text $frontTracker -Heading 'Активные фронты'
    $activeFrontIds = [regex]::Matches($activeFrontSection, '(?m)^\|\s*(FRONT-[A-Z0-9-]+)\s*\|') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    $urgentFrontSection = Get-SectionText -Text $frontTracker -Heading 'Срочные развилки'
    $urgentFrontIds = [regex]::Matches($urgentFrontSection, '(?m)^\|\s*(FRONT-[A-Z0-9-]+)\s*\|') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique

    $timerSection = Get-SectionText -Text $frontTracker -Heading 'Таймеры угроз'
    $timerRows = @(
        $timerSection -split "\r?\n" |
            ForEach-Object { Convert-MarkdownTableRow -Line $_ } |
            Where-Object { $null -ne $_ -and $_.Count -ge 2 -and $_[0] -match '^FRONT-[A-Z0-9-]+$' }
    )
    $timerKeys = @(
        $timerRows |
            ForEach-Object { "$($_[0])|$($_[1])" }
    )

    $sceneFrontIds = @()
    if ($filesByType.ContainsKey('scene')) {
        $sceneFrontIds = @(
            $filesByType['scene'] |
                ForEach-Object {
                    $sceneText = Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName
                    if ($sceneText -match '(?m)^front_id:\s*(FRONT-[A-Z0-9-]+)\s*$') {
                        $Matches[1]
                    }
                } |
                Sort-Object -Unique
        )
    }

    $locationFrontIds = @()
    if ($filesByType.ContainsKey('location')) {
        $locationFrontIds = @(
            $filesByType['location'] |
                ForEach-Object {
                    $relativeLocation = (Get-RelativePath $_.FullName) -replace '\\', '/'
                    $locationText = Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName
                    if ($locationText -notmatch '(?m)^front_id:\s*(\S+)\s*$') {
                        Add-Problem Error "Location has no front_id field: $relativeLocation"
                        return
                    }

                    $locationFrontId = $Matches[1].Trim()
                    if ([string]::IsNullOrWhiteSpace($locationFrontId)) {
                        Add-Problem Error "Location has empty front_id field: $relativeLocation"
                        return
                    }

                    if ($locationFrontId -eq '-') {
                        return
                    }

                    if ($locationFrontId -notmatch '^FRONT-[A-Z0-9-]+$') {
                        Add-Problem Error "Location has invalid front_id format: $relativeLocation -> $locationFrontId"
                        return
                    }

                    if ($declaredFrontIdSet -notcontains $locationFrontId) {
                        Add-Problem Error "Location uses undeclared FRONT-ID: $relativeLocation -> $locationFrontId"
                        return
                    }

                    $locationFrontId
                } |
                Sort-Object -Unique
        )
    }

    $decisionFrontIds = @()
    if ($filesByType.ContainsKey('decision_log')) {
        $decisionText = Get-Content -Raw -Encoding UTF8 -LiteralPath $filesByType['decision_log'][0].FullName
        $decisionFrontIds = @(
            [regex]::Matches($decisionText, '\bFRONT-[A-Z0-9-]+\b') |
                ForEach-Object { $_.Value } |
                Sort-Object -Unique
        )
    }

    $questionFrontIds = @()
    if ($filesByType.ContainsKey('open_questions')) {
        $questionText = Get-Content -Raw -Encoding UTF8 -LiteralPath $filesByType['open_questions'][0].FullName
        $questionFrontIds = @(
            [regex]::Matches($questionText, '\bFRONT-[A-Z0-9-]+\b') |
                ForEach-Object { $_.Value } |
                Sort-Object -Unique
        )
    }

    $frontTrackerLinkedIds = @(
        $frontTracker -split "\r?\n" |
            Where-Object {
                $_ -match '\bFRONT-[A-Z0-9-]+\b' -and
                $_ -match '`(?:01_Кампания/Ветки|01_Кампания/02_Журнал_решений|01_Кампания/03_Нерешенные_вопросы|04_Локации)/[^`]+\.md`'
            } |
            ForEach-Object {
                [regex]::Matches($_, '\bFRONT-[A-Z0-9-]+\b') |
                    ForEach-Object { $_.Value }
            } |
            Sort-Object -Unique
    )

    foreach ($id in $activeFrontIds) {
        $hasDirectLink =
            $sceneFrontIds -contains $id -or
            $locationFrontIds -contains $id -or
            $decisionFrontIds -contains $id -or
            $questionFrontIds -contains $id

        $hasTrackerProjectLink = $frontTrackerLinkedIds -contains $id

        if (-not ($hasDirectLink -or $hasTrackerProjectLink)) {
            Add-Problem Warning "Active FRONT-ID has no scene/question/decision/location link: $id"
        }
    }

    $frontRegistryPath = Join-Path $root '09_Реестры\Фронты.json'
    if (Test-Path -LiteralPath $frontRegistryPath) {
        $frontRegistry = $null
        try {
            $frontRegistry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $frontRegistryPath) | ConvertFrom-Json
        } catch {
            Add-Problem Error "Front registry is not valid JSON: 09_Реестры\Фронты.json - $($_.Exception.Message)"
        }

        if ($null -ne $frontRegistry) {
            if ($frontRegistry.type -ne 'front_registry') {
                Add-Problem Error "Front registry has invalid type: $($frontRegistry.type)"
            }

            $registryFrontIds = @($frontRegistry.fronts | ForEach-Object { $_.id })
            $registryUrgentIds = @($frontRegistry.urgent_forks | ForEach-Object { $_.id })
            $registryActiveIds = @($frontRegistry.active_fronts | ForEach-Object { $_.id })
            $registryTimerKeys = @($frontRegistry.timers | ForEach-Object { "$($_.id)|$($_.timer)" })

            if ($registryFrontIds.Count -eq 0) {
                Add-Problem Error 'Front registry contains no FRONT-ID declarations.'
            }

            foreach ($id in ($registryFrontIds | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })) {
                Add-Problem Error "Duplicate FRONT-ID in registry: $id"
            }

            foreach ($id in $registryFrontIds) {
                if ($id -notmatch '^FRONT-[A-Z0-9-]+$') {
                    Add-Problem Error "Invalid FRONT-ID in registry: $id"
                }
            }

            foreach ($id in ($registryUrgentIds + $registryActiveIds + (@($frontRegistry.timers) | ForEach-Object { $_.id }) | Sort-Object -Unique)) {
                if ($registryFrontIds -notcontains $id) {
                    Add-Problem Error "Front registry uses undeclared FRONT-ID: $id"
                }
            }

            foreach ($priority in @($frontRegistry.urgent_forks | ForEach-Object { $_.priority })) {
                if ($priority -notin @('критический', 'высокий', 'средний', 'низкий')) {
                    Add-Problem Error "Invalid front priority in registry: $priority"
                }
            }

            Compare-IdSets 'front registry declarations' $registryFrontIds 'front_tracker declarations' $declaredFrontIds
            Compare-IdSets 'front registry urgent forks' $registryUrgentIds 'front_tracker urgent forks' $urgentFrontIds
            Compare-IdSets 'front registry active fronts' $registryActiveIds 'front_tracker active fronts' $activeFrontIds
            Compare-IdSets 'front registry timers' $registryTimerKeys 'front_tracker timers' $timerKeys
        }
    }
}

if ($filesByType.ContainsKey('scene')) {
    foreach ($file in $filesByType['scene']) {
        $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        $relativeFile = Get-RelativePath $file.FullName

        if ($text -notmatch '(?m)^front_id:\s*(\S+)\s*$') {
            Add-Problem Error "Scene has no front_id field: $relativeFile"
            continue
        }

        $frontId = $Matches[1].Trim()

        if ([string]::IsNullOrWhiteSpace($frontId)) {
            Add-Problem Error "Scene has empty front_id field: $relativeFile"
            continue
        }

        if ($frontId -eq '-') {
            if ($text -match '(?m)^status:\s*active\s*$') {
                Add-Problem Warning "Active scene has front_id '-': $relativeFile"
            }

            continue
        }

        if ($frontId -notmatch '^FRONT-[A-Z0-9-]+$') {
            Add-Problem Error "Scene has invalid front_id format: $relativeFile -> $frontId"
            continue
        }

        if ($declaredFrontIdSet.Count -gt 0 -and $declaredFrontIdSet -notcontains $frontId) {
            Add-Problem Error "Scene uses undeclared FRONT-ID: $relativeFile -> $frontId"
        }
    }
}

$result = [pscustomobject]@{
    MarkdownFiles = $mdFiles.Count
    ImageFiles = $imageFiles.Count
    Errors = $errors.Count
    Warnings = $warnings.Count
}

$result | Format-List

if ($warnings.Count -gt 0) {
    "`nWarnings:"
    $warnings | Sort-Object | ForEach-Object { "- $_" }
}

if ($errors.Count -gt 0) {
    "`nErrors:"
    $errors | Sort-Object | ForEach-Object { "- $_" }
    exit 1
}

"`nProject check completed successfully."
}
