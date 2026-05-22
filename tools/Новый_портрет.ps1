param(
    [Parameter(Mandatory = $true)]
    [string]$Character,

    [string]$ImagePath,

    [string]$Prompt = '',

    [string]$Reference = '',

    [string]$OutputName,

    [switch]$PrepareOnly,

    [switch]$Force,

    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$characterRoot = Join-Path $root '03_Персонажи'
$portraitRoot = Join-Path $root '11_Медиа\Портреты_персонажей'

function Convert-ToSlug {
    param([string]$Value)

    return (($Value.Trim() -replace '[\\/:*?"<>|]', '') -replace '\s+', '_')
}

function Get-RelativeProjectPath {
    param([string]$Path)

    return (($Path.Substring($root.Length).TrimStart('\', '/')) -replace '\\', '/')
}

function Get-ImageSize {
    param([string]$Path)

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
}

function Test-PortraitRatio {
    param(
        [int]$Width,
        [int]$Height
    )

    if ($Height -le 0 -or $Width -le 0) {
        return $false
    }

    $ratio = $Width / $Height
    $isThreeByFour = [math]::Abs($ratio - 0.75) -le 0.04
    return $isThreeByFour
}

function Set-OrAppendFrontMatterField {
    param(
        [string]$Text,
        [string]$Field,
        [string]$Value
    )

    if ($Text -match "(?m)^$([regex]::Escape($Field)):\s*.*$") {
        return [regex]::Replace($Text, "(?m)^$([regex]::Escape($Field)):\s*.*$", "${Field}: $Value", 1)
    }

    return [regex]::Replace($Text, "(?m)^---\s*$", "---`r`n${Field}: $Value", 1)
}

function Update-PortraitSection {
    param(
        [string]$Text,
        [string]$ImageReference,
        [string]$PromptReference
    )

    $section = @(
        '## Портрет',
        '',
        '- Статус: есть.',
        "- Основной файл: ``$ImageReference``",
        "- Промпт: ``$PromptReference``"
    ) -join "`r`n"

    if ($Text -match '(?ms)^## Портрет\r?\n.*?(?=\r?\n## |\z)') {
        return [regex]::Replace($Text, '(?ms)^## Портрет\r?\n.*?(?=\r?\n## |\z)', $section.TrimEnd(), 1)
    }

    return $Text.TrimEnd() + "`r`n`r`n" + $section.TrimEnd() + "`r`n"
}

if (-not $ImagePath -and -not $PrepareOnly) {
    throw 'ImagePath is required. If you only want to prepare the folder and prompt, use -PrepareOnly.'
}

$slug = Convert-ToSlug $Character
$characterPath = Join-Path $characterRoot "$slug.md"

if (-not (Test-Path -LiteralPath $characterPath)) {
    $matches = Get-ChildItem -LiteralPath $characterRoot -File -Filter '*.md' |
        Where-Object {
            $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName
            $text -match "(?m)^#\s+$([regex]::Escape($Character.Trim()))\s*$"
        }

    if (@($matches).Count -ne 1) {
        throw "Cannot find exactly one character card for '$Character'."
    }

    $characterPath = $matches[0].FullName
    $slug = [System.IO.Path]::GetFileNameWithoutExtension($matches[0].Name)
}

$characterText = Get-Content -Raw -Encoding UTF8 -LiteralPath $characterPath
$displayName = $Character.Trim()
if ($characterText -match '(?m)^#\s+(.+?)\s*$') {
    $displayName = $Matches[1].Trim()
}

$targetFolder = Join-Path $portraitRoot $slug
if (-not (Test-Path -LiteralPath $targetFolder)) {
    New-Item -ItemType Directory -Path $targetFolder | Out-Null
}

$promptPath = Join-Path $targetFolder 'Промпт_портрета.md'
$targetImagePath = $null
$relativeImage = $null

if ($ImagePath) {
    $resolvedImage = (Resolve-Path -LiteralPath $ImagePath).Path
    $extension = [System.IO.Path]::GetExtension($resolvedImage).ToLowerInvariant()

    if ($extension -notmatch '^\.(jpg|jpeg|png|webp)$') {
        throw "Unsupported image extension: $extension"
    }

    $size = Get-ImageSize -Path $resolvedImage
    if (-not (Test-PortraitRatio -Width $size.Width -Height $size.Height)) {
        throw "Portrait must be 3:4. Actual size: $($size.Width)x$($size.Height)."
    }

    if (-not $OutputName) {
        $OutputName = "${slug}_основной_портрет$extension"
    }

    if ([System.IO.Path]::GetExtension($OutputName) -eq '') {
        $OutputName = "$OutputName$extension"
    }

    $targetImagePath = Join-Path $targetFolder $OutputName

    if ((Test-Path -LiteralPath $targetImagePath) -and -not $Force) {
        throw "Target portrait already exists: $(Get-RelativeProjectPath $targetImagePath). Use -Force to replace it."
    }

    Copy-Item -LiteralPath $resolvedImage -Destination $targetImagePath -Force:$Force
    $relativeImage = Get-RelativeProjectPath $targetImagePath
}

$relativePrompt = Get-RelativeProjectPath $promptPath

if (-not $Prompt.Trim()) {
    $Prompt = "Вертикальный кинематографичный реалистичный портрет персонажа: $displayName. Формат 3:4, персонаж крупно в кадре, лицо хорошо видно, высокая детализация лица и глаз. Одежда, статус, фон и символика должны следовать карточке персонажа. Атмосфера: героическое темное фэнтези, серьезное эпическое настроение. Без текста, логотипов, рамок и водяных знаков."
}

$referenceLine = if ($Reference.Trim()) { "- Пользовательский референс: $Reference" } else { "- Референс не предоставлен; портрет строится по карточке персонажа, канону и стилю проекта." }
$status = if ($relativeImage) { 'used' } else { 'planned' }
$resultLine = if ($relativeImage) { "result: $relativeImage" } else { 'result: null' }
$resultBullet = if ($relativeImage) { "- Основной файл: ``$relativeImage``" } else { '- Основной файл: null' }

$promptContent = @(
    "# Промпт портрета: $displayName",
    '',
    '---',
    'type: portrait_prompt',
    "status: $status",
    'canon_level: support',
    "character: $displayName",
    $resultLine,
    '---',
    '',
    '## Назначение',
    '',
    'Основной портрет персонажа для карточки и индекса портретов.',
    '',
    '## Референсы',
    '',
    $referenceLine,
    '- `00_Инструкции_для_ИИ/02_Портреты_персонажей.md`',
    "- ``$(Get-RelativeProjectPath $characterPath)``",
    '',
    '## Готовый промпт',
    '',
    '```text',
    $Prompt,
    '```',
    '',
    '## Итог',
    '',
    $resultBullet,
    '- Формат: 3:4.'
) -join "`r`n"

Set-Content -LiteralPath $promptPath -Encoding UTF8 -Value $promptContent

if ($relativeImage) {
    $characterText = Set-OrAppendFrontMatterField -Text $characterText -Field 'portrait' -Value $relativeImage
    $characterText = Set-OrAppendFrontMatterField -Text $characterText -Field 'portrait_status' -Value 'available'
    $characterText = Update-PortraitSection -Text $characterText -ImageReference $relativeImage -PromptReference $relativePrompt
    Set-Content -LiteralPath $characterPath -Encoding UTF8 -Value $characterText

    $characterIndexPath = Join-Path $characterRoot '00_Индекс_персонажей.md'
    $characterIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $characterIndexPath
    $characterRelative = Get-RelativeProjectPath $characterPath
    $escapedCharacterRelative = [regex]::Escape($characterRelative)
    $characterIndexPattern = '(?m)^(\|\s*[^|]+\|\s*[^|]+\|\s*)([^|]+?)(\s*\|\s*`' + $escapedCharacterRelative + '`\s*\|\s*)$'
    if ($characterIndex -match $characterIndexPattern) {
        $characterIndex = [regex]::Replace($characterIndex, $characterIndexPattern, "`${1}есть`${3}", 1)
        Set-Content -LiteralPath $characterIndexPath -Encoding UTF8 -Value $characterIndex
    }

    $portraitIndexPath = Join-Path $portraitRoot 'Индекс_портретов.md'
    $portraitIndex = Get-Content -Raw -Encoding UTF8 -LiteralPath $portraitIndexPath
    $escapedName = [regex]::Escape($displayName)
    $portraitLines = $portraitIndex -split "\r?\n" |
        Where-Object { $_ -notmatch "^\|\s*$escapedName\s*\|" }
    $portraitIndex = $portraitLines -join "`r`n"
    $portraitRow = "| $displayName | есть | ``$relativeImage`` |"
    $availableTablePattern = '(?ms)(## Портреты есть.*?^\| --- \| --- \| --- \|\s*)'
    $portraitIndex = [regex]::Replace(
        $portraitIndex,
        $availableTablePattern,
        "`${1}`r`n$portraitRow`r`n",
        [System.Text.RegularExpressions.RegexOptions]::Multiline,
        [TimeSpan]::FromSeconds(1)
    )
    Set-Content -LiteralPath $portraitIndexPath -Encoding UTF8 -Value $portraitIndex
}

if (-not $SkipCheck) {
    & (Join-Path $root 'tools\Проверить_портреты.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & (Join-Path $root 'tools\Проверить_проект.ps1')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if ($relativeImage) {
    "Created portrait: $relativeImage"
} else {
    "Prepared portrait prompt: $relativePrompt"
}
}
