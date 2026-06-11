param(
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("wmma_tool_test_" + [guid]::NewGuid().ToString('N'))
$copyRoot = Join-Path $tempRoot 'project'

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action
    )

    "`n== $Name =="
    & $Action
    if (-not $? -or $LASTEXITCODE -ne 0) {
        throw "Step failed: $Name"
    }

    $global:LASTEXITCODE = 0
}

function Assert-TextContains {
    param(
        [string]$Path,
        [string]$Expected
    )

    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
    if (-not $text.Contains($Expected)) {
        throw "Expected text was not found in $Path`: $Expected"
    }
}

function Assert-TextNotContains {
    param(
        [string]$Path,
        [string]$Unexpected
    )

    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
    if ($text.Contains($Unexpected)) {
        throw "Unexpected text was found in $Path`: $Unexpected"
    }
}

function Get-NextAcceptedDecisionId {
    param([string]$DecisionLogPath)

    $decisionLog = Get-Content -Raw -Encoding UTF8 -LiteralPath $DecisionLogPath
    $numbers = @(
        [regex]::Matches($decisionLog, '(?m)^###\s+DEC-(\d{3})\s*$') |
            ForEach-Object { [int]$_.Groups[1].Value }
    )

    if ($numbers.Count -eq 0) {
        return 'DEC-001'
    }

    $max = [int](($numbers | Measure-Object -Maximum).Maximum)
    return ('DEC-{0:D3}' -f ($max + 1))
}

function Ensure-ToolTestPendingDecision {
    param(
        [string]$DecisionRegistryPath,
        [string]$ToolsRoot
    )

    $testQuestion = 'Тестовое pending-решение для проверки инструментов'
    $registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $DecisionRegistryPath) | ConvertFrom-Json
    $existing = @(
        $registry.decisions |
            Where-Object {
                ($_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*') -and
                $_.question -eq $testQuestion
            }
    ) | Select-Object -First 1

    if ($null -ne $existing) {
        return $existing.id
    }

    $toolOutput = & (Join-Path $ToolsRoot 'Новое_решение.ps1') `
        -Question $testQuestion `
        -Owner 'Инструменты' `
        -PlayerCharacter 'Тест / Инструменты' `
        -Scene 'Проверка инструментов' `
        -StoryDate 'тест инструментов' `
        -Choice 'ожидает решения' `
        -PlayerAddition 'временная запись, созданная только в копии проекта для проверки инструментов.' `
        -ImmediateEffect 'ожидает решения' `
        -LongTermConsequences 'тестовая запись должна закрыться инструментом.' `
        -Links 'tools/Проверить_инструменты.ps1' `
        -SkipCheck

    $toolOutput | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $registry = (Get-Content -Raw -Encoding UTF8 -LiteralPath $DecisionRegistryPath) | ConvertFrom-Json
    $created = @(
        $registry.decisions |
            Where-Object {
                ($_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*') -and
                $_.question -eq $testQuestion
            }
    ) | Select-Object -Last 1

    if ($null -eq $created) {
        throw 'Новое_решение.ps1 did not add the tool test pending decision to the decision registry.'
    }

    return $created.id
}

function Invoke-ExpectedFailure {
    param(
        [string]$Name,
        [scriptblock]$Action
    )

    $failed = $false
    try {
        & $Action
    } catch {
        $failed = $true
    }

    if (-not $failed) {
        throw "Expected failure did not happen: $Name"
    }

    $global:LASTEXITCODE = 0
}

try {
    New-Item -ItemType Directory -Path $copyRoot -Force | Out-Null

    Invoke-Step 'Копия проекта во временную папку' {
        $robocopyArgs = @(
            $root,
            $copyRoot,
            '/E',
            '/XD',
            '.git',
            '/NFL',
            '/NDL',
            '/NJH',
            '/NJS',
            '/NP'
        )

        & robocopy @robocopyArgs | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy failed with exit code $LASTEXITCODE"
        }

        $global:LASTEXITCODE = 0
    }

    $toolsRoot = Join-Path $copyRoot 'tools'
    $inboxPath = Join-Path $copyRoot '07_Черновики_и_идеи\Входящие_сообщения.md'
    $frontTrackerPath = Join-Path $copyRoot '01_Кампания\06_Фронты_и_таймеры.md'
    $frontRegistryPath = Join-Path $copyRoot '09_Реестры\Фронты.json'
    $sourceIndexPath = Join-Path $copyRoot '08_Источники\00_Индекс_источников.md'
    $sceneIndexPath = Join-Path $copyRoot '01_Кампания\00_Индекс_сцен.md'
    $decisionLogPath = Join-Path $copyRoot '01_Кампания\02_Журнал_решений.md'
    $openQuestionsPath = Join-Path $copyRoot '01_Кампания\03_Нерешенные_вопросы.md'
    $closedQuestionsPath = Join-Path $copyRoot '01_Кампания\03_Закрытые_вопросы.md'
    $decisionRegistryPath = Join-Path $copyRoot '09_Реестры\Решения.json'
    $questionRegistryPath = Join-Path $copyRoot '09_Реестры\Вопросы.json'
    $currentContextPath = Join-Path $copyRoot '01_Кампания\00_Текущий_контекст.md'

    Invoke-Step 'Принять входящее с источником' {
        & (Join-Path $toolsRoot 'Принять_сообщение.ps1') `
            -Title 'Тестовое входящее инструментов' `
            -Text 'Тестовый текст для проверки инструментов.' `
            -Mode source `
            -SkipCheck

        Assert-TextContains -Path $inboxPath -Expected 'Тестовое входящее инструментов'
        Assert-TextContains -Path $inboxPath -Expected '08_Источники/'
    }

    Invoke-Step 'Обработать входящее' {
        & (Join-Path $toolsRoot 'Обработать_входящее.ps1') `
            -Title 'Тестовое входящее инструментов' `
            -Summary 'Тестовое входящее обработано инструментом.' `
            -Links 'tools/README.md' `
            -SkipCheck

        Assert-TextContains -Path $inboxPath -Expected 'Статус: обработано.'
        Assert-TextContains -Path $inboxPath -Expected 'Тестовое входящее обработано инструментом.'
    }

    Invoke-Step 'Создать новый фронт' {
        & (Join-Path $toolsRoot 'Новый_фронт.ps1') `
            -Id FRONT-TOOL-TEST `
            -Name 'Тестовый фронт инструментов' `
            -Description 'тестовая линия проверки инструментов' `
            -Participants 'Тестовые участники' `
            -State 'Тестовое состояние' `
            -Risk 'Тестовый риск' `
            -Trigger 'Тестовый триггер' `
            -Links '01_Кампания/Ветки/Александрос/Сцена_011_Багровый_Пик_и_возвращение_Эрин.md' `
            -Timer 'Тестовый таймер' `
            -SkipCheck

        Assert-TextContains -Path $frontTrackerPath -Expected 'FRONT-TOOL-TEST'
        $frontRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $frontRegistryPath) | ConvertFrom-Json
        if (@($frontRegistryAfter.fronts | Where-Object { $_.id -eq 'FRONT-TOOL-TEST' }).Count -ne 1) {
            throw 'Новый_фронт.ps1 did not add FRONT-TOOL-TEST to the front registry.'
        }
    }

    Invoke-Step 'Обновить фронт' {
        & (Join-Path $toolsRoot 'Обновить_фронт.ps1') `
            -FrontId FRONT-TOOL-TEST `
            -State 'Тестовое состояние после обновления фронта' `
            -Risk 'Тестовый риск после обновления фронта' `
            -NextTrigger 'Тестовый триггер после обновления фронта' `
            -TimerStatus 'активен: тестовый таймер обновлен' `
            -TimerTrigger 'Тестовое срабатывание таймера после обновления' `
            -SkipCheck

        Assert-TextContains -Path $frontTrackerPath -Expected 'Тестовое состояние после обновления фронта'
        Assert-TextContains -Path $frontTrackerPath -Expected 'активен: тестовый таймер обновлен'
        $frontRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $frontRegistryPath) | ConvertFrom-Json
        $updatedFront = @($frontRegistryAfter.active_fronts | Where-Object { $_.id -eq 'FRONT-TOOL-TEST' })[0]
        $updatedTimer = @($frontRegistryAfter.timers | Where-Object { $_.id -eq 'FRONT-TOOL-TEST' })[0]
        if ($updatedFront.state -ne 'Тестовое состояние после обновления фронта') {
            throw 'Обновить_фронт.ps1 did not update active front state in registry.'
        }

        if ($updatedTimer.status -ne 'активен: тестовый таймер обновлен') {
            throw 'Обновить_фронт.ps1 did not update timer status in registry.'
        }
    }

    Invoke-Step 'Создать новый вопрос' {
        & (Join-Path $toolsRoot 'Новый_вопрос.ps1') `
            -Scope chapter `
            -Text 'Тестовый вопрос инструментов' `
            -Owner 'Инструменты' `
            -Priority 'высокий' `
            -Status waiting `
            -SkipCheck

        Assert-TextContains -Path $openQuestionsPath -Expected 'Тестовый вопрос инструментов'
        $questionRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath) | ConvertFrom-Json
        if (@($questionRegistryAfter.questions | Where-Object { $_.text -eq 'Тестовый вопрос инструментов' }).Count -ne 1) {
            throw 'Новый_вопрос.ps1 did not add the test question to the question registry.'
        }
    }

    $toolTestPendingId = Ensure-ToolTestPendingDecision `
        -DecisionRegistryPath $decisionRegistryPath `
        -ToolsRoot $toolsRoot

    Assert-TextContains -Path $decisionLogPath -Expected 'временная запись, созданная только в копии проекта для проверки инструментов.'
    Assert-TextContains -Path $openQuestionsPath -Expected 'Тестовое pending-решение для проверки инструментов'
    Assert-TextContains -Path $currentContextPath -Expected 'Тестовое pending-решение для проверки инструментов'

    Invoke-Step 'Закрыть pending-решение не портит файлы при неизвестном ID' {
        $decisionRegistryBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionRegistryPath
        $decisionLogBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
        $openQuestionsBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $missingPendingId = 'DEC-PENDING-999'
        $registryBefore = $decisionRegistryBefore | ConvertFrom-Json
        $registryIds = @($registryBefore.decisions | ForEach-Object { $_.id })
        if ($registryIds -contains $missingPendingId) {
            $missingPendingId = 'DEC-PENDING-998'
        }

        if ($registryIds -contains $missingPendingId) {
            throw 'Cannot find unused DEC-PENDING-* ID for failure test.'
        }

        Invoke-ExpectedFailure -Name 'Закрыть_решение.ps1 with unknown pending decision ID' -Action {
            & (Join-Path $toolsRoot 'Закрыть_решение.ps1') `
                -PendingId $missingPendingId `
                -AcceptedId 'DEC-999' `
                -Choice "Тестовое закрытие $missingPendingId" `
                -Effect "Тестовый эффект закрытия $missingPendingId" `
                -SkipCheck
        }

        $decisionRegistryAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionRegistryPath
        $decisionLogAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
        $openQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        if ($decisionRegistryAfter -ne $decisionRegistryBefore) {
            throw 'Закрыть_решение.ps1 changed decision registry before failing.'
        }

        if ($decisionLogAfter -ne $decisionLogBefore) {
            throw 'Закрыть_решение.ps1 changed decision log before failing.'
        }

        if ($openQuestionsAfter -ne $openQuestionsBefore) {
            throw 'Закрыть_решение.ps1 changed open questions before failing.'
        }
    }

    Invoke-Step 'Закрыть pending-решение без порчи журнала' {
        $decisionLogBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
        $pendingId = $toolTestPendingId
        if ($decisionLogBefore -notmatch "(?m)^###\s+$([regex]::Escape($pendingId))\s*$") {
            throw "Tool test pending decision was not found for Закрыть_решение.ps1 test: $pendingId"
        }

        $acceptedId = Get-NextAcceptedDecisionId -DecisionLogPath $decisionLogPath
        $choice = "Тестовое закрытие $pendingId"
        $effect = "Тестовый эффект закрытия $pendingId"

        & (Join-Path $toolsRoot 'Закрыть_решение.ps1') `
            -PendingId $pendingId `
            -AcceptedId $acceptedId `
            -Choice $choice `
            -Effect $effect `
            -SkipCheck

        $decisionLogAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionLogPath
        $openQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $currentContextAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $currentContextPath
        $decisionRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $decisionRegistryPath) | ConvertFrom-Json
        $acceptedRegistryDecision = @($decisionRegistryAfter.decisions | Where-Object { $_.id -eq $acceptedId })[0]

        Assert-TextContains -Path $decisionLogPath -Expected "### $acceptedId"
        Assert-TextContains -Path $decisionLogPath -Expected "Выбор: $choice"
        Assert-TextContains -Path $decisionLogPath -Expected "Немедленный эффект: $effect"
        Assert-TextNotContains -Path $decisionLogPath -Unexpected "### $pendingId"
        Assert-TextNotContains -Path $openQuestionsPath -Unexpected 'resolved${4}'
        Assert-TextNotContains -Path $openQuestionsPath -Unexpected $pendingId

        if ($currentContextAfter -match "\b$([regex]::Escape($pendingId))\b") {
            throw "Current context still contains closed pending decision $pendingId."
        }

        if ($decisionLogAfter -notmatch '(?ms)## Формат записи.+?Выбор:\s*\r?\n.+?Немедленный эффект:\s*\r?\n') {
            throw 'Decision log format block was unexpectedly changed.'
        }

        if (([regex]::Matches($decisionLogAfter, [regex]::Escape($choice))).Count -ne 1) {
            throw 'Choice replacement touched more than the accepted decision block.'
        }

        if ($null -eq $acceptedRegistryDecision -or $acceptedRegistryDecision.state -ne 'accepted') {
            throw "Decision registry was not updated as accepted: $acceptedId"
        }

        if (@($decisionRegistryAfter.decisions | Where-Object { $_.id -eq $pendingId }).Count -ne 0) {
            throw "Decision registry still contains pending decision: $pendingId"
        }
    }

    Invoke-Step 'Закрыть вопрос не портит файлы при неизвестном ID' {
        $questionRegistryBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath
        $openQuestionsBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $closedQuestionsBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $closedQuestionsPath

        $missingQuestionId = 'Q-C3-999'
        $registryBefore = $questionRegistryBefore | ConvertFrom-Json
        $registryIds = @($registryBefore.questions | ForEach-Object { $_.id })
        if ($registryIds -contains $missingQuestionId) {
            $missingQuestionId = 'Q-WORLD-999'
        }

        if ($registryIds -contains $missingQuestionId) {
            throw 'Cannot find unused Q-* ID for failure test.'
        }

        Invoke-ExpectedFailure -Name 'Закрыть_вопрос.ps1 with unknown question ID' -Action {
            & (Join-Path $toolsRoot 'Закрыть_вопрос.ps1') `
                -QuestionId $missingQuestionId `
                -Resolution "Тестовое закрытие $missingQuestionId" `
                -SkipCheck
        }

        $questionRegistryAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath
        $openQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $closedQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $closedQuestionsPath

        if ($questionRegistryAfter -ne $questionRegistryBefore) {
            throw 'Закрыть_вопрос.ps1 changed the question registry before failing.'
        }

        if ($openQuestionsAfter -ne $openQuestionsBefore) {
            throw 'Закрыть_вопрос.ps1 changed open questions before failing.'
        }

        if ($closedQuestionsAfter -ne $closedQuestionsBefore) {
            throw 'Закрыть_вопрос.ps1 changed closed questions before failing.'
        }
    }

    Invoke-Step 'Закрыть открытый вопрос без порчи истории' {
        $openQuestionsBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $questionMatch = [regex]::Match($openQuestionsBefore, '(?m)^\|\s*(Q-(?:C\d+|WORLD)-\d{3})\s*\|(?:[^|]*\|){3}\s*(?:active|waiting|later)\s*\|\s*$')
        if (-not $questionMatch.Success) {
            throw 'No open Q-C*/Q-WORLD entry found for Закрыть_вопрос.ps1 test.'
        }

        $questionId = $questionMatch.Groups[1].Value
        $resolution = "Тестовое закрытие $questionId"
        $today = Get-Date -Format 'yyyy-MM-dd'
        $historyExpected = '- {0} - `{1}`: {2}' -f $today, $questionId, $resolution

        & (Join-Path $toolsRoot 'Закрыть_вопрос.ps1') `
            -QuestionId $questionId `
            -Resolution $resolution `
            -SkipCheck

        $openQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $closedQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $closedQuestionsPath
        $questionRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath) | ConvertFrom-Json
        $registryQuestion = @($questionRegistryAfter.questions | Where-Object { $_.id -eq $questionId })[0]
        $registryHistory = @($questionRegistryAfter.history | Where-Object { $_.id -eq $questionId -and $_.resolution -eq $resolution })

        Assert-TextNotContains -Path $openQuestionsPath -Unexpected '$QuestionId'
        Assert-TextNotContains -Path $openQuestionsPath -Unexpected 'resolved${'

        Assert-TextContains -Path $closedQuestionsPath -Expected $historyExpected
        Assert-TextNotContains -Path $closedQuestionsPath -Unexpected '$QuestionId'
        Assert-TextNotContains -Path $closedQuestionsPath -Unexpected 'resolved${'

        if ($openQuestionsAfter -match "(?m)^\|\s*$([regex]::Escape($questionId))\s*\|") {
            throw "Closed question row still exists in open questions: $questionId"
        }

        if ($closedQuestionsAfter -notmatch "(?m)^\|\s*$([regex]::Escape($questionId))\s*\|(?:[^|]*\|){3}\s*resolved\s*\|\s*$") {
            throw "Question row was not moved to closed questions as resolved: $questionId"
        }

        if ($null -eq $registryQuestion -or $registryQuestion.status -ne 'resolved') {
            throw "Question registry was not updated as resolved: $questionId"
        }

        if ($registryHistory.Count -ne 1) {
            throw "Question registry history was not updated once for: $questionId"
        }
    }

    Invoke-Step 'Параллельное закрытие вопросов не портит таблицы' {
        $openQuestionsBefore = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $questionIds = @(
            [regex]::Matches($openQuestionsBefore, '(?m)^\|\s*(Q-(?:C\d+|WORLD)-\d{3})\s*\|(?:[^|]*\|){3}\s*(?:active|waiting|later)\s*\|\s*$') |
                ForEach-Object { $_.Groups[1].Value } |
                Select-Object -First 2
        )

        if ($questionIds.Count -lt 2) {
            'Skipped parallel close test: not enough open questions in temporary copy.'
            return
        }

        $jobs = foreach ($questionId in $questionIds) {
            Start-Job -ScriptBlock {
                param(
                    [string]$ToolsRoot,
                    [string]$QuestionId
                )

                & (Join-Path $ToolsRoot 'Закрыть_вопрос.ps1') `
                    -QuestionId $QuestionId `
                    -Resolution "Параллельное тестовое закрытие $QuestionId" `
                    -SkipCheck
            } -ArgumentList $toolsRoot, $questionId
        }

        try {
            Wait-Job -Job $jobs | Out-Null
            foreach ($job in $jobs) {
                $output = Receive-Job -Job $job -ErrorAction Stop
                if ($job.State -ne 'Completed') {
                    throw "Parallel close job failed: $($job.State) $output"
                }
            }
        } finally {
            Remove-Job -Job $jobs -Force -ErrorAction SilentlyContinue
        }

        $openQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $openQuestionsPath
        $closedQuestionsAfter = Get-Content -Raw -Encoding UTF8 -LiteralPath $closedQuestionsPath
        $questionRegistryAfter = (Get-Content -Raw -Encoding UTF8 -LiteralPath $questionRegistryPath) | ConvertFrom-Json
        foreach ($questionId in $questionIds) {
            if ($openQuestionsAfter -match "(?m)^\|\s*$([regex]::Escape($questionId))\s*\|") {
                throw "Parallel close left question in open table: $questionId"
            }

            if ($closedQuestionsAfter -notmatch "(?m)^\|\s*$([regex]::Escape($questionId))\s*\|(?:[^|]*\|){3}\s*resolved\s*\|\s*$") {
                throw "Parallel close did not move question to closed table: $questionId"
            }

            $registryQuestion = @($questionRegistryAfter.questions | Where-Object { $_.id -eq $questionId })[0]
            if ($null -eq $registryQuestion -or $registryQuestion.status -ne 'resolved') {
                throw "Parallel close did not resolve registry question: $questionId"
            }
        }
    }

    Invoke-Step 'Создать сцену' {
        & (Join-Path $toolsRoot 'Новая_сцена.ps1') `
            -Branch 'Тестовая_ветка_инструментов' `
            -Title 'Проверка инструментов' `
            -FrontId FRONT-TOOL-TEST `
            -Status active `
            -Summary 'Тестовая сцена для проверки инструментов.' `
            -SkipCheck

        Assert-TextContains -Path $sceneIndexPath -Expected 'FRONT-TOOL-TEST'
    }

    Invoke-Step 'Создать сцену из входящего' {
        & (Join-Path $toolsRoot 'Сцена_из_входящего.ps1') `
            -Branch 'Тестовая_ветка_инструментов' `
            -Title 'Входящее для сцены инструментов' `
            -Text 'Текст входящего, которое должно стать сценой.' `
            -FrontId FRONT-TOOL-TEST `
            -SkipCheck

        Assert-TextContains -Path $inboxPath -Expected 'Входящее для сцены инструментов'
        Assert-TextContains -Path $inboxPath -Expected 'Создана новая сцена'
    }

    Invoke-Step 'Создать персонажа и локацию' {
        & (Join-Path $toolsRoot 'Новый_персонаж.ps1') `
            -Name 'Тестовый Персонаж Инструментов' `
            -Role 'тест инструментов' `
            -SkipCheck

        & (Join-Path $toolsRoot 'Новая_локация.ps1') `
            -Name 'Тестовая Локация Инструментов' `
            -Summary 'тестовая локация инструментов' `
            -FrontId FRONT-TOOL-TEST `
            -SkipCheck

        Assert-TextContains -Path (Join-Path $copyRoot '03_Персонажи\00_Индекс_персонажей.md') -Expected 'Тестовый Персонаж Инструментов'
        Assert-TextContains -Path (Join-Path $copyRoot '04_Локации\00_Индекс_локаций.md') -Expected 'Тестовая Локация Инструментов'
    }

    Invoke-Step 'Закрепить тестовый портрет' {
        Add-Type -AssemblyName System.Drawing
        $testPortraitPath = Join-Path $tempRoot 'portrait_test.png'
        $bitmap = [System.Drawing.Bitmap]::new(300, 400)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([System.Drawing.Color]::FromArgb(40, 60, 90))
            $bitmap.Save($testPortraitPath, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }

        & (Join-Path $toolsRoot 'Новый_портрет.ps1') `
            -Character 'Тестовый Персонаж Инструментов' `
            -ImagePath $testPortraitPath `
            -SkipCheck

        Assert-TextContains -Path (Join-Path $copyRoot '03_Персонажи\Тестовый_Персонаж_Инструментов.md') -Expected 'portrait_status: available'
        Assert-TextContains -Path (Join-Path $copyRoot '11_Медиа\Портреты_персонажей\Индекс_портретов.md') -Expected 'Тестовый Персонаж Инструментов'
    }

    Invoke-Step 'Собрать индекс источников' {
        & (Join-Path $toolsRoot 'Собрать_индекс_источников.ps1') -SkipCheck
        Assert-TextContains -Path $sourceIndexPath -Expected 'Тестовое входящее инструментов'
        Assert-TextContains -Path $sourceIndexPath -Expected 'Входящее для сцены инструментов'
    }

    Invoke-Step 'Финальная проверка временной копии' {
        & (Join-Path $toolsRoot 'Собрать_индекс_сцен.ps1') -SkipCheck
        & (Join-Path $toolsRoot 'Собрать_решения.ps1') -SkipCheck
        & (Join-Path $toolsRoot 'Собрать_вопросы.ps1') -SkipCheck
        & (Join-Path $toolsRoot 'Собрать_фронты.ps1') -SkipCheck
        & (Join-Path $toolsRoot 'Собрать_панель_хода.ps1') -SkipCheck
        & (Join-Path $toolsRoot 'Проверить_реестры.ps1')
        & (Join-Path $toolsRoot 'Проверить_проект.ps1')
    }

    "`nTool check completed successfully in: $copyRoot"
} finally {
    if ($KeepTemp) {
        "`nTemp copy kept: $copyRoot"
    } elseif (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
}
