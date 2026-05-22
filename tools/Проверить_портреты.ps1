$ErrorActionPreference = 'Stop'

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()


. (Join-Path $PSScriptRoot '_lib.ps1')
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Invoke-WmmaToolMain -Root $root -Name $MyInvocation.MyCommand.Name -ScriptBlock {
$characterRoot = Join-Path $root '03_Персонажи'
$portraitRoot = Join-Path $root '11_Медиа\Портреты_персонажей'
$errors = New-Object 'System.Collections.Generic.List[string]'
$missing = New-Object 'System.Collections.Generic.List[string]'
$wrongRatio = New-Object 'System.Collections.Generic.List[string]'
$available = 0
$planned = 0

function Get-RelativePath {
    param([string]$Path)

    if ($Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($root.Length).TrimStart('\', '/')
    }

    return $Path
}

function Test-ProjectPath {
    param([string]$Reference)

    $normalized = $Reference -replace '/', '\'
    if ([System.IO.Path]::IsPathRooted($normalized)) {
        return Test-Path -LiteralPath $normalized
    }

    return Test-Path -LiteralPath (Join-Path $root $normalized)
}

function Resolve-ProjectPath {
    param([string]$Reference)

    $normalized = $Reference -replace '/', '\'
    if ([System.IO.Path]::IsPathRooted($normalized)) {
        if (Test-Path -LiteralPath $normalized) {
            return (Resolve-Path -LiteralPath $normalized).Path
        }

        return $null
    }

    $candidate = Join-Path $root $normalized
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
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

$characterFiles = Get-ChildItem -LiteralPath $characterRoot -File -Filter '*.md' |
    Where-Object { $_.Name -ne '00_Индекс_персонажей.md' -and $_.Name -ne '00_Словарь_имен_и_алиасов.md' }

foreach ($file in $characterFiles) {
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    if ($text -notmatch '(?m)^type:\s*character\s*$') {
        continue
    }

    $name = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)

    if ($text -notmatch '(?m)^portrait_status:\s*(available|missing|planned)\s*$') {
        $errors.Add("Invalid or missing portrait_status: $(Get-RelativePath $file.FullName)") | Out-Null
        continue
    }

    $status = $Matches[1]
    switch ($status) {
        'available' { $available++ }
        'planned' { $planned++ }
        'missing' { $missing.Add($name) | Out-Null }
    }

    if ($status -eq 'available') {
        if ($text -notmatch '(?m)^portrait:\s*(.+)\s*$') {
            $errors.Add("Missing portrait path: $(Get-RelativePath $file.FullName)") | Out-Null
            continue
        }

        $portraitPath = $Matches[1].Trim()
        $portraitFullPath = Resolve-ProjectPath -Reference $portraitPath
        if ($portraitPath -eq 'null' -or -not $portraitFullPath) {
            $errors.Add("portrait_status=available but file is missing: $(Get-RelativePath $file.FullName)") | Out-Null
        } else {
            $size = Get-ImageSize -Path $portraitFullPath
            if (-not $size) {
                $errors.Add("Cannot read portrait dimensions: $(Get-RelativePath $portraitFullPath)") | Out-Null
            } elseif (-not (Test-AcceptedPortraitRatio -Width $size.Width -Height $size.Height)) {
                $wrongRatio.Add("$(Get-RelativePath $portraitFullPath) ($($size.Width)x$($size.Height))") | Out-Null
            }
        }
    }
}

$portraitFoldersWithImages = Get-ChildItem -LiteralPath $portraitRoot -Directory |
    Where-Object {
        (Get-ChildItem -LiteralPath $_.FullName -File |
            Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|webp)$' } |
            Measure-Object).Count -gt 0
    }

$foldersWithoutPrompt = foreach ($folder in $portraitFoldersWithImages) {
    $prompt = Join-Path $folder.FullName 'Промпт_портрета.md'
    if (-not (Test-Path -LiteralPath $prompt)) {
        Get-RelativePath $folder.FullName
    }
}

[pscustomobject]@{
    Characters = $characterFiles.Count
    PortraitAvailable = $available
    PortraitPlanned = $planned
    PortraitMissing = $missing.Count
    PortraitFoldersWithImages = $portraitFoldersWithImages.Count
    FoldersWithoutPrompt = @($foldersWithoutPrompt).Count
    WrongAspectRatio = $wrongRatio.Count
    Errors = $errors.Count
} | Format-List

if ($missing.Count -gt 0) {
    "`nMissing portraits:"
    $missing | Sort-Object | ForEach-Object { "- $_" }
}

if (@($foldersWithoutPrompt).Count -gt 0) {
    "`nPortrait folders without prompt:"
    $foldersWithoutPrompt | Sort-Object | ForEach-Object { "- $_" }
}

if ($wrongRatio.Count -gt 0) {
    "`nPortraits with invalid aspect ratio:"
    $wrongRatio | Sort-Object | ForEach-Object { "- $_" }
    foreach ($item in $wrongRatio) {
        $errors.Add("Invalid portrait aspect ratio: $item") | Out-Null
    }
}

if ($errors.Count -gt 0) {
    "`nErrors:"
    $errors | Sort-Object | ForEach-Object { "- $_" }
    exit 1
}
}
