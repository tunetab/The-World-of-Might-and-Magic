[CmdletBinding()]
param(
    [switch]$SkipCheck,

    [switch]$Only
)

$ErrorActionPreference = 'Stop'
if($Only){$SkipCheck=$true}

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$decisionLogPath = Join-Path $root '01_Кампания\02_Журнал_решений.md'

function Format-Value {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return ''
    }

    return (($Value.ToString() -replace '\r?\n', ' ').Trim())
}

function Add-DecisionBlock {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [object]$Decision
    )

    $Lines.Add("### $($Decision.id)")
    $Lines.Add('')
    $Lines.Add("Дата в реальности: $(Format-Value $Decision.real_date)")
    $Lines.Add("Дата в сюжете: $(Format-Value $Decision.story_date)")
    $Lines.Add("Игрок / персонаж: $(Format-Value $Decision.player_character)")
    $Lines.Add("Сцена: $(Format-Value $Decision.scene)")
    $Lines.Add("Выбор: $(Format-Value $Decision.choice)")
    $Lines.Add("Дополнение игрока: $(Format-Value $Decision.player_addition)")
    $Lines.Add("Немедленный эффект: $(Format-Value $Decision.immediate_effect)")
    $Lines.Add("Долгосрочные последствия: $(Format-Value $Decision.long_term_consequences)")
    $Lines.Add("Связанные файлы: $(Format-Value $Decision.links)")
    $Lines.Add("Статус: $(Format-Value $Decision.status_text)")
}

function Render-DecisionLog {
    param([object]$Registry)

    $decisions = @($Registry.decisions)
    $pendingDecisions = @(
        $decisions |
            Where-Object { $_.state -eq 'pending' -or $_.id -like 'DEC-PENDING-*' } |
            Sort-Object @{ Expression = { [int]($_.id -replace '^DEC-PENDING-', '') } }
    )
    $acceptedDecisions = @(
        $decisions |
            Where-Object { $_.state -eq 'accepted' -and $_.id -match '^DEC-\d{3}$' } |
            Sort-Object @{ Expression = { [int]($_.id -replace '^DEC-', '') } }
    )
    $lines = New-Object 'System.Collections.Generic.List[string]'

    $lines.Add('# Журнал решений')
    $lines.Add('')
    $lines.Add('---')
    $lines.Add('type: decision_log')
    $lines.Add('status: active')
    $lines.Add('canon_level: active')
    $lines.Add("updated_real_date: $($Registry.updated_real_date)")
    $lines.Add('generated_by: tools/Собрать_решения.ps1')
    $lines.Add('source_registry: 09_Реестры/Решения.json')
    $lines.Add('---')
    $lines.Add('')
    $lines.Add('Этот файл пересобирается из `09_Реестры/Решения.json`. Для создания `DEC-PENDING-*` используй `.\tools\Новое_решение.ps1`, для закрытия - `.\tools\Закрыть_решение.ps1`.')
    $lines.Add('')
    $lines.Add('## Формат записи')
    $lines.Add('')
    $lines.Add('```text')
    $lines.Add('ID:')
    $lines.Add('Дата в реальности:')
    $lines.Add('Дата в сюжете:')
    $lines.Add('Игрок / персонаж:')
    $lines.Add('Сцена:')
    $lines.Add('Выбор:')
    $lines.Add('Дополнение игрока:')
    $lines.Add('Немедленный эффект:')
    $lines.Add('Долгосрочные последствия:')
    $lines.Add('Связанные файлы:')
    $lines.Add('Статус:')
    $lines.Add('```')
    $lines.Add('')
    $lines.Add('## Ожидают решения')
    foreach ($decision in $pendingDecisions) {
        $lines.Add('')
        Add-DecisionBlock -Lines $lines -Decision $decision
    }

    $lines.Add('')
    $lines.Add('## Принятые решения')
    foreach ($decision in $acceptedDecisions) {
        $lines.Add('')
        Add-DecisionBlock -Lines $lines -Decision $decision
    }

    Write-Utf8NoBom -Path $decisionLogPath -Text (($lines -join "`n").TrimEnd() + "`n")
}

$registry = Read-WmmaRegistry $root 'Решения'

Render-DecisionLog -Registry $registry

if (-not $Only) {
    & (Join-Path $root 'tools\Собрать_вопросы.ps1') -SkipCheck
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
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

"Updated decision registry and Markdown log: 09_Реестры/Решения.json"
}
