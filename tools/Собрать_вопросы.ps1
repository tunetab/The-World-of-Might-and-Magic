param(
    [switch]$ImportFromMarkdown,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$today = Get-Date -Format 'yyyy-MM-dd'
$registryDir = Join-Path $root '09_Реестры'
$registryPath = Join-Path $registryDir 'Вопросы.json'
$openQuestionsPath = Join-Path $root '01_Кампания\03_Нерешенные_вопросы.md'
$closedQuestionsPath = Join-Path $root '01_Кампания\03_Закрытые_вопросы.md'

function Read-Text {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }

    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Text
    )

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
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

function Get-TableRows {
    param(
        [string]$Text,
        [string]$Heading
    )

    $section = Get-SectionText -Text $Text -Heading $Heading
    $rows = New-Object 'System.Collections.Generic.List[object]'

    foreach ($line in ($section -split "\r?\n")) {
        $cells = Convert-MarkdownTableRow -Line $line
        if ($null -ne $cells -and $cells[0] -notin @('ID', 'Приоритет')) {
            $rows.Add($cells) | Out-Null
        }
    }

    return $rows
}

function ConvertTo-QuestionObject {
    param(
        [object[]]$Cells,
        [string]$Scope
    )

    if ($Cells.Count -lt 5) {
        throw "Question row has too few columns: $($Cells -join ' | ')"
    }

    return [pscustomobject][ordered]@{
        id = $Cells[0]
        priority = $Cells[1]
        text = $Cells[2]
        owner = $Cells[3]
        scope = $Scope
        status = $Cells[4]
    }
}

function Get-HistoryRows {
    param([string]$ClosedQuestions)

    $history = New-Object 'System.Collections.Generic.List[object]'
    $matches = [regex]::Matches(
        $ClosedQuestions,
        '(?m)^-\s+(\d{4}-\d{2}-\d{2})\s+-\s+`(Q-(?:C\d+|WORLD)-\d{3})`:\s+(.+?)\s*$'
    )

    foreach ($match in $matches) {
        $history.Add([pscustomobject][ordered]@{
            date = $match.Groups[1].Value
            id = $match.Groups[2].Value
            resolution = $match.Groups[3].Value.Trim()
        }) | Out-Null
    }

    return $history
}

function Get-CurrentChapterFromMarkdown {
    param([string]$Text)

    if ($Text -match '(?m)^current_chapter:\s*(\d+)\s*$') {
        return [int]$Matches[1]
    }

    return 3
}

function Get-ChapterQuestionsHeading {
    param([int]$Chapter)

    return "Вопросы главы $Chapter"
}

function Get-ChapterQuestionRows {
    param(
        [string]$Text,
        [int]$Chapter
    )

    $heading = Get-ChapterQuestionsHeading -Chapter $Chapter
    $rows = @(Get-TableRows -Text $Text -Heading $heading)
    if ($rows.Count -gt 0) {
        return $rows
    }

    $fallback = [regex]::Match($Text, '(?m)^##\s+(Вопросы главы \d+)\s*$')
    if ($fallback.Success) {
        return @(Get-TableRows -Text $Text -Heading $fallback.Groups[1].Value)
    }

    return @()
}

function Import-QuestionRegistry {
    $openQuestions = Read-Text -Path $openQuestionsPath
    $closedQuestions = Read-Text -Path $closedQuestionsPath
    $questions = New-Object 'System.Collections.Generic.List[object]'
    $currentChapter = Get-CurrentChapterFromMarkdown -Text $openQuestions

    foreach ($row in (Get-ChapterQuestionRows -Text $openQuestions -Chapter $currentChapter)) {
        $questions.Add((ConvertTo-QuestionObject -Cells $row -Scope 'chapter')) | Out-Null
    }

    foreach ($row in (Get-TableRows -Text $openQuestions -Heading 'Вопросы по миру')) {
        $questions.Add((ConvertTo-QuestionObject -Cells $row -Scope 'world')) | Out-Null
    }

    foreach ($row in (Get-ChapterQuestionRows -Text $closedQuestions -Chapter $currentChapter)) {
        $question = ConvertTo-QuestionObject -Cells $row -Scope 'chapter'
        $question.status = 'resolved'
        $questions.Add($question) | Out-Null
    }

    foreach ($row in (Get-TableRows -Text $closedQuestions -Heading 'Вопросы по миру')) {
        $question = ConvertTo-QuestionObject -Cells $row -Scope 'world'
        $question.status = 'resolved'
        $questions.Add($question) | Out-Null
    }

    $historyRows = Get-HistoryRows -ClosedQuestions $closedQuestions

    return [pscustomobject][ordered]@{
        type = 'question_registry'
        status = 'active'
        canon_level = 'support'
        current_chapter = $currentChapter
        updated_real_date = $today
        generated_from = @(
            '01_Кампания/03_Нерешенные_вопросы.md',
            '01_Кампания/03_Закрытые_вопросы.md'
        )
        questions = @($questions.ToArray())
        history = @($historyRows)
    }
}

function Read-QuestionRegistry {
    if (-not (Test-Path -LiteralPath $registryPath)) {
        throw "Question registry is missing: 09_Реестры/Вопросы.json. Run .\tools\Собрать_вопросы.ps1 -ImportFromMarkdown once."
    }

    $json = Read-Text -Path $registryPath
    return $json | ConvertFrom-Json
}

function Save-QuestionRegistry {
    param([object]$Registry)

    if (-not (Test-Path -LiteralPath $registryDir)) {
        New-Item -ItemType Directory -Path $registryDir -Force | Out-Null
    }

    $json = ($Registry | ConvertTo-Json -Depth 8)
    Write-Utf8NoBom -Path $registryPath -Text ($json.TrimEnd() + "`n")
}

function Format-MarkdownCell {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return ''
    }

    return (($Value.ToString() -replace '\|', '/') -replace '\r?\n', ' ').Trim()
}

function Add-RawLine {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Line = ''
    )

    $Lines.Add($Line) | Out-Null
}

function Add-QuestionTable {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string[]]$Header,
        [object[]]$Questions
    )

    Add-RawLine -Lines $Lines -Line ('| ' + ($Header -join ' | ') + ' |')
    Add-RawLine -Lines $Lines -Line ('| ' + (($Header | ForEach-Object { '---' }) -join ' | ') + ' |')
    foreach ($question in $Questions) {
        Add-RawLine -Lines $Lines -Line ('| ' + (@(
            Format-MarkdownCell $question.id
            Format-MarkdownCell $question.priority
            Format-MarkdownCell $question.text
            Format-MarkdownCell $question.owner
            Format-MarkdownCell $question.status
        ) -join ' | ') + ' |')
    }
}

function Get-ActiveDecisionRows {
    $decisionRegistryPath = Join-Path $root '09_Реестры\Решения.json'
    if (Test-Path -LiteralPath $decisionRegistryPath) {
        $decisionRegistry = (Read-Text -Path $decisionRegistryPath) | ConvertFrom-Json
        $rows = New-Object 'System.Collections.Generic.List[object]'

        foreach ($decision in @($decisionRegistry.decisions)) {
            if ($decision.id -notmatch '^DEC-PENDING-\d{3}$' -and $decision.state -ne 'pending') {
                continue
            }

            $rows.Add(@(
                $decision.id
                $(if ([string]::IsNullOrWhiteSpace($decision.priority)) { 'высокий' } else { $decision.priority })
                $(if ([string]::IsNullOrWhiteSpace($decision.question)) { $decision.choice } else { $decision.question })
                $(if ([string]::IsNullOrWhiteSpace($decision.owner)) { $decision.player_character } else { $decision.owner })
                $(if ([string]::IsNullOrWhiteSpace($decision.panel_status)) { 'active' } else { $decision.panel_status })
            )) | Out-Null
        }

        return $rows
    }

    $openQuestions = Read-Text -Path $openQuestionsPath
    $rows = New-Object 'System.Collections.Generic.List[object]'

    foreach ($row in (Get-TableRows -Text $openQuestions -Heading 'Активные решения')) {
        if ($row.Count -ge 5 -and $row[0] -match '^DEC-PENDING-\d{3}$') {
            $rows.Add($row) | Out-Null
        }
    }

    return $rows
}

function Add-DecisionTable {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [object[]]$Rows
    )

    Add-RawLine -Lines $Lines -Line '| ID | Приоритет | Вопрос | Владелец / ветка | Статус |'
    Add-RawLine -Lines $Lines -Line '| --- | --- | --- | --- | --- |'
    foreach ($row in $Rows) {
        Add-RawLine -Lines $Lines -Line ('| ' + (@(
            Format-MarkdownCell $row[0]
            Format-MarkdownCell $row[1]
            Format-MarkdownCell $row[2]
            Format-MarkdownCell $row[3]
            Format-MarkdownCell $row[4]
        ) -join ' | ') + ' |')
    }
}

function Render-OpenQuestions {
    param([object]$Registry)

    $decisionRows = @(Get-ActiveDecisionRows)
    $questions = @($Registry.questions)
    $chapterNumber = [int]$Registry.current_chapter
    $chapterPrefix = "Q-C$chapterNumber"
    $chapterHeading = Get-ChapterQuestionsHeading -Chapter $chapterNumber
    $chapterQuestions = @($questions | Where-Object { $_.scope -eq 'chapter' -and $_.status -ne 'resolved' })
    $worldQuestions = @($questions | Where-Object { $_.scope -eq 'world' -and $_.status -ne 'resolved' })
    $lines = New-Object 'System.Collections.Generic.List[string]'

    Add-RawLine -Lines $lines -Line '# Нерешенные вопросы'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines -Line 'type: open_questions'
    Add-RawLine -Lines $lines -Line 'status: active'
    Add-RawLine -Lines $lines -Line 'canon_level: active'
    Add-RawLine -Lines $lines -Line "current_chapter: $($Registry.current_chapter)"
    Add-RawLine -Lines $lines -Line "updated_real_date: $($Registry.updated_real_date)"
    Add-RawLine -Lines $lines -Line 'generated_by: tools/Собрать_вопросы.ps1'
    Add-RawLine -Lines $lines -Line 'source_registry: 09_Реестры/Вопросы.json'
    Add-RawLine -Lines $lines -Line 'decision_registry: 09_Реестры/Решения.json'
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Этот файл пересобирается из `09_Реестры/Вопросы.json`; активные `DEC-PENDING-*` берутся из `09_Реестры/Решения.json`. Для создания вопросов используй `.\tools\Новый_вопрос.ps1`, для закрытия вопросов - `.\tools\Закрыть_вопрос.ps1`.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line "Новый ``$chapterPrefix-*`` или ``Q-WORLD-*`` всегда получает следующий свободный номер по максимуму из ``09_Реестры/Вопросы.json``. Не переиспользуй ID закрытого вопроса."
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Как читать приоритет'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '- `критический` - решение нужно для ближайшего хода или может резко изменить карту.'
    Add-RawLine -Lines $lines -Line '- `высокий` - активный сюжетный узел текущей главы.'
    Add-RawLine -Lines $lines -Line '- `средний` - важное последствие, которое можно раскрыть после ближайших решений.'
    Add-RawLine -Lines $lines -Line '- `низкий` - лор или фон, который стоит уточнить позже.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Как читать статус'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '- `active` - ближайший блокирующий узел или решение, которое нужно держать перед глазами.'
    Add-RawLine -Lines $lines -Line '- `waiting` - важный вопрос главы, который ждет сцены, решения или нового источника.'
    Add-RawLine -Lines $lines -Line '- `later` - мировой или фоновый бэклог без давления на ближайший ход.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Закрытый вопрос получает статус `resolved` в JSON-реестре и выводится в `01_Кампания/03_Закрытые_вопросы.md`; в этом файле остаются только `active`, `waiting` и `later`.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Активные решения'
    Add-RawLine -Lines $lines
    Add-DecisionTable -Lines $lines -Rows $decisionRows
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line "## $chapterHeading"
    Add-RawLine -Lines $lines
    Add-QuestionTable -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Владелец / ветка', 'Статус') -Questions $chapterQuestions
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Вопросы по миру'
    Add-RawLine -Lines $lines
    Add-QuestionTable -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Область', 'Статус') -Questions $worldQuestions

    Write-Utf8NoBom -Path $openQuestionsPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

function Render-ClosedQuestions {
    param([object]$Registry)

    $questions = @($Registry.questions)
    $chapterNumber = [int]$Registry.current_chapter
    $chapterPrefix = "Q-C$chapterNumber"
    $chapterHeading = Get-ChapterQuestionsHeading -Chapter $chapterNumber
    $chapterQuestions = @($questions | Where-Object { $_.scope -eq 'chapter' -and $_.status -eq 'resolved' })
    $worldQuestions = @($questions | Where-Object { $_.scope -eq 'world' -and $_.status -eq 'resolved' })
    $history = @($Registry.history)
    $lines = New-Object 'System.Collections.Generic.List[string]'

    Add-RawLine -Lines $lines -Line '# Закрытые вопросы'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines -Line 'type: closed_questions'
    Add-RawLine -Lines $lines -Line 'status: active'
    Add-RawLine -Lines $lines -Line 'canon_level: support'
    Add-RawLine -Lines $lines -Line "current_chapter: $($Registry.current_chapter)"
    Add-RawLine -Lines $lines -Line "updated_real_date: $($Registry.updated_real_date)"
    Add-RawLine -Lines $lines -Line 'generated_by: tools/Собрать_вопросы.ps1'
    Add-RawLine -Lines $lines -Line 'source_registry: 09_Реестры/Вопросы.json'
    Add-RawLine -Lines $lines -Line '---'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line 'Этот файл пересобирается из `09_Реестры/Вопросы.json` и хранит вопросы, которые уже получили канонический ответ. Открытые вопросы и pending-решения остаются в `01_Кампания/03_Нерешенные_вопросы.md`.'
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line "Новые ``$chapterPrefix-*`` и ``Q-WORLD-*`` создаются через ``.\tools\Новый_вопрос.ps1``; закрытый ID не переиспользуется."
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line "## $chapterHeading"
    Add-RawLine -Lines $lines
    Add-QuestionTable -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Владелец / ветка', 'Статус') -Questions $chapterQuestions
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## Вопросы по миру'
    Add-RawLine -Lines $lines
    Add-QuestionTable -Lines $lines -Header @('ID', 'Приоритет', 'Вопрос', 'Область', 'Статус') -Questions $worldQuestions
    Add-RawLine -Lines $lines
    Add-RawLine -Lines $lines -Line '## История закрытия вопросов'
    foreach ($item in $history) {
        Add-RawLine -Lines $lines
        Add-RawLine -Lines $lines -Line ('- {0} - `{1}`: {2}' -f $item.date, $item.id, (Format-MarkdownCell $item.resolution))
    }

    Write-Utf8NoBom -Path $closedQuestionsPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

if ($ImportFromMarkdown) {
    $registry = Import-QuestionRegistry
    Save-QuestionRegistry -Registry $registry
} else {
    $registry = Read-QuestionRegistry
}

$registry.updated_real_date = $today
Render-OpenQuestions -Registry $registry
Render-ClosedQuestions -Registry $registry
Save-QuestionRegistry -Registry $registry

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

"Updated question registry and Markdown views: 09_Реестры/Вопросы.json"
}
