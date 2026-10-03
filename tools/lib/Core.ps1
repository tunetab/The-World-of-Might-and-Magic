# Shared primitives. All paths are relative to the explicit project root.
function Read-WmmaText {
    param([string]$Path)
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

function Write-WmmaText {
    param([string]$Path, [AllowEmptyString()][string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
    if ((Test-Path -LiteralPath $Path) -and (Read-WmmaText $Path) -ceq $Text) { return }
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $Path) }
}

function Read-WmmaJson {
    param([string]$Path)
    return (Read-WmmaText $Path | ConvertFrom-Json)
}

function Read-WmmaRegistry {
    param([string]$Root,[string]$Name)
    $path=Resolve-WmmaPath $Root "09_Реестры/$Name.json"
    if(-not (Test-Path -LiteralPath $path)){
        throw "Registry missing: $Name.json. Restore the authoritative JSON from a verified backup."
    }
    return Read-WmmaJson $path
}

function Write-WmmaJson {
    param([string]$Path, [object]$Value)
    Write-WmmaText $Path (($Value | ConvertTo-Json -Depth 50).TrimEnd() + "`n")
}

function Get-WmmaHash {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Resolve-WmmaPath {
    param([string]$Root, [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path)) { throw "Expected a relative project path: $Path" }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath((Join-Path $base $Path))
    if (-not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Path leaves project: $Path" }
    $cursor = $full
    while ($cursor -ne $base) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -Force -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse points are not writable project paths: $Path" }
        $cursor = Split-Path -Parent $cursor
    }
    return $full
}

function Get-WmmaRelativePath {
    param([string]$Root, [string]$Path)
    return $Path.Substring($Root.TrimEnd('\', '/').Length).TrimStart('\', '/').Replace('\', '/')
}

function Get-WmmaMeta {
    param([string]$Text, [string]$Field, [AllowNull()][object]$Default = '')
    $block = [regex]::Match($Text, '(?s)\A(?:\uFEFF)?# [^\r\n]+\r?\n\s*---\r?\n(.*?)\r?\n---')
    if ($block.Success) {
        $entry = [regex]::Match($block.Groups[1].Value, '(?m)^' + [regex]::Escape($Field) + ':[ \t]*([^\r\n]*)')
        if ($entry.Success) { return $entry.Groups[1].Value.Trim() }
    }
    return $Default
}

function Get-WmmaTitle {
    param([string]$Text)
    $heading=[regex]::Match($Text,'(?m)^#\s+(.+?)\s*$')
    if($heading.Success){return $heading.Groups[1].Value.Trim()}
    return 'Без названия'
}

function Convert-WmmaTableRow {
    param([string]$Line)
    if($Line -notmatch '^\|.+\|$' -or $Line -match '^\|\s*-'){return $null}
    return ,($Line.Trim('|') -split '\|' | ForEach-Object {$_.Trim()})
}

function Format-WmmaTableCell {
    param([AllowNull()][object]$Value,[string]$Empty='')
    if($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)){return $Empty}
    return (([string]$Value -replace '\|','/') -replace '\r?\n',' ').Trim()
}

function Convert-WmmaFileName {
    param([string]$Value,[switch]$Lowercase)
    $safe=([regex]::Replace($Value.Trim(),'\s+','_') -replace '[\\/:*?"<>|]','').Trim('_','.',' ')
    if(-not $safe){throw 'Cannot build a file name from an empty title.'}
    if($Lowercase){return $safe.ToLowerInvariant()}
    return $safe
}

function Set-WmmaMeta {
    param([string]$Text, [string]$Field, [string]$Value)
    $block = [regex]::Match($Text, '(?s)\A(?:\uFEFF)?# [^\r\n]+\r?\n\s*---\r?\n(.*?)\r?\n---')
    if (-not $block.Success) { throw 'Document has no project front matter.' }
    $meta = $block.Groups[1].Value
    $pattern = '(?m)^' + [regex]::Escape($Field) + ':[^\r\n]*'
    if ([regex]::IsMatch($meta, $pattern)) { $meta = [regex]::Replace($meta, $pattern, [Text.RegularExpressions.MatchEvaluator]{ param($m) "${Field}: $Value" }) }
    else { $meta += "`n${Field}: $Value" }
    return $Text.Substring(0, $block.Groups[1].Index) + $meta + $Text.Substring($block.Groups[1].Index + $block.Groups[1].Length)
}

function Get-WmmaSection {
    param([string]$Text, [string]$Heading)
    $section=@(Get-WmmaMarkdownSections $Text 2 | Where-Object {$_.heading -ceq $Heading})|Select-Object -First 1
    if ($section) { return $section.text.Trim() }
    return ''
}

# A generated view changes only when its contents change. Keep the previous
# generation date and bytes if a rebuild merely proposes a newer date.
function Write-WmmaGeneratedText {
    param([string]$Path,[string]$Text,[ValidateSet('markdown','json')][string]$Format='markdown')
    if(Test-Path -LiteralPath $Path){
        $previous=Read-WmmaText $Path
        $pattern=if($Format -eq 'json'){'(?m)^  "updated_real_date": "\d{4}-\d{2}-\d{2}"'}else{'(?m)^generated_real_date: \d{4}-\d{2}-\d{2}'}
        $before=[regex]::Replace($previous.Replace("`r`n","`n"),$pattern,'<generation-date>')
        $after=[regex]::Replace($Text.Replace("`r`n","`n"),$pattern,'<generation-date>')
        if($before -ceq $after){return}
    }
    Write-WmmaText $Path $Text
}

function Get-WmmaMarkdownSections {
    param([string]$Text,[int]$Level=2)
    $headings=[Collections.Generic.List[object]]::new();$fenceChar='';$fenceLength=0
    foreach($line in [regex]::Matches($Text,'(?m)^[^\r\n]*(?:\r?\n|$)')){
        if(-not $line.Length){continue}
        $value=$line.Value.TrimEnd([char]13,[char]10)
        $fence=[regex]::Match($value,'^ {0,3}(`{3,}|~{3,})(.*)$')
        if($fence.Success){
            $marker=$fence.Groups[1].Value
            if(-not $fenceChar){$fenceChar=[string]$marker[0];$fenceLength=$marker.Length}
            elseif([string]$marker[0] -eq $fenceChar -and $marker.Length -ge $fenceLength -and -not $fence.Groups[2].Value.Trim()){$fenceChar='';$fenceLength=0}
            continue
        }
        if($fenceChar){continue}
        $h=[regex]::Match($value,'^(#{1,6})[ \t]+(.+?)[ \t]*$')
        if($h.Success){$headings.Add([pscustomobject]@{level=$h.Groups[1].Value.Length;heading=$h.Groups[2].Value;index=$line.Index;body_start=$line.Index+$line.Length})}
    }
    for($i=0;$i -lt $headings.Count;$i++){
        $h=$headings[$i];if($h.level -ne $Level){continue}
        $end=$Text.Length
        for($j=$i+1;$j -lt $headings.Count;$j++){if($headings[$j].level -le $Level){$end=$headings[$j].index;break}}
        [pscustomobject]@{heading=$h.heading;index=$h.index;body_start=$h.body_start;end=$end;text=$Text.Substring($h.body_start,$end-$h.body_start)}
    }
}

function Get-WmmaInboxEntries {
    param([string]$Text)
    foreach($section in @(Get-WmmaMarkdownSections $Text 3)){
        $value=$Text.Substring($section.index,$section.end-$section.index)
        [pscustomobject]@{Value=$value;Index=$section.index;Length=$value.Length;Groups=@([pscustomobject]@{Value=$value},[pscustomobject]@{Value=$section.heading},[pscustomobject]@{Value=$section.text})}
    }
}

function Get-WmmaEntryField {
    param([string]$Text,[string]$Field)
    $fence=[regex]::Match($Text,'(?m)^ {0,3}(?:`{3,}|~{3,})')
    $header=if($fence.Success){$Text.Substring(0,$fence.Index)}else{$Text}
    $match=[regex]::Match($header,'(?m)^'+[regex]::Escape($Field)+':[ \t]*([^\r\n]*)')
    if($match.Success){return $match.Groups[1].Value.Trim()}
    return ''
}

function Get-WmmaTextFence {
    param([string]$Text)
    $max=2;foreach($run in [regex]::Matches($Text,'`+')){$max=[Math]::Max($max,$run.Length)}
    return ('`'*($max+1))
}

function Get-WmmaRawMessage {
    param([string]$Text)
    $match=[regex]::Match($Text,'(?ms)^ {0,3}(?<fence>`{3,}|~{3,})text[ \t]*\r?\n(?<body>.*?)\r?\n {0,3}\k<fence>[ \t]*(?:\r?\n|$)')
    if(-not $match.Success){return $null}
    return $match.Groups['body'].Value
}

function Test-WmmaSourcePayload {
    param([string]$Text,[string]$Hash)
    $raw=Get-WmmaRawMessage $Text
    return $null -ne $raw -and (Get-WmmaHash ($raw.Replace("`r`n","`n").Trim())) -ceq $Hash
}

function Assert-WmmaSingleLine {
    param([string]$Value,[string]$Name,[switch]$AllowEmpty)
    if($Value -match '[\r\n]' -or (-not $AllowEmpty -and [string]::IsNullOrWhiteSpace($Value))){throw "$Name must be a single nonempty line."}
}

function Assert-WmmaReceiptPayload {
    param([string]$Root,[object]$Receipt)
    if($Receipt.source_path){
        $source=Read-WmmaText (Resolve-WmmaPath $Root $Receipt.source_path)
        if((Get-WmmaMeta $source 'request_id') -cne $Receipt.request_id -or -not (Test-WmmaSourcePayload $source $Receipt.content_sha256)){throw 'Receipt source payload is missing or changed.'}
    }else{
        $inbox=Read-WmmaText (Join-Path $Root '07_Черновики_и_идеи/Входящие_сообщения.md')
        $entries=@(Get-WmmaInboxEntries $inbox|Where-Object {(Get-WmmaEntryField $_.Value 'Request-ID') -ceq $Receipt.request_id})
        if($entries.Count -ne 1 -or -not (Test-WmmaSourcePayload $entries[0].Value $Receipt.content_sha256)){throw 'Receipt inbox payload is missing, duplicated or changed.'}
    }
    if($Receipt.scene_path){
        $scene=Read-WmmaText (Resolve-WmmaPath $Root $Receipt.scene_path)
        if((Get-WmmaMeta $scene 'request_id') -cne $Receipt.request_id){throw 'Receipt points to another scene.'}
    }
}

function Get-WmmaImageSize {
    param([string]$Path)
    $stream=$null;$image=$null
    try{
        Add-Type -AssemblyName System.Drawing
        $stream=[IO.File]::OpenRead($Path)
        $image=[Drawing.Image]::FromStream($stream)
        return [pscustomobject]@{Width=$image.Width;Height=$image.Height}
    }catch{return $null}finally{if($image){$image.Dispose()};if($stream){$stream.Dispose()}}
}

function Get-WmmaArrayMeta {
    param([string]$Text, [string]$Field)
    $value = Get-WmmaMeta $Text $Field
    if (-not $value -or $value -in @('null', '-')) { return @() }
    if ($value.StartsWith('[')) { return @($value | ConvertFrom-Json) }
    return @($value)
}

function Get-WmmaCurrentChapter {
    param([string]$Root)
    $active = @(Get-ChildItem -LiteralPath (Join-Path $Root '01_Кампания/Главы') -Filter '*.md' -File | Where-Object { (Get-WmmaMeta (Read-WmmaText $_.FullName) 'status') -eq 'active' })
    if ($active.Count -ne 1) { throw "Expected exactly one active chapter, found $($active.Count)." }
    return [int](Get-WmmaMeta (Read-WmmaText $active[0].FullName) 'chapter')
}

function Get-WmmaDocuments {
    param([string]$Root)
    foreach ($directory in @('01_Кампания/Ветки', '02_Лор', '03_Персонажи', '04_Локации', '05_Активы_персонажей', '08_Источники')) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Root $directory) -Recurse -File -Filter '*.md') {
            if ($file.FullName -match '[\\/]История[\\/]') { continue }
            $text = Read-WmmaText $file.FullName
            $type = Get-WmmaMeta $text 'type'
            if ($type -in @('character','location','character_asset','scene','source','source_note','source_compilation','lore','faction_index','economy_notes','timeline')) {
                [pscustomobject]@{path=(Get-WmmaRelativePath $Root $file.FullName); full_path=$file.FullName; text=$text; type=$type; id=(Get-WmmaMeta $text 'id')}
            }
        }
    }
}

function Convert-WmmaCell {
    param([object]$Value)
    return ([string]$Value).Replace('|', '&#124;').Replace("`r", '').Replace("`n", '<br>')
}

function Get-WmmaPriority {
    param([string]$Value)
    switch ($Value) { 'критический' { 4 } 'высокий' { 3 } 'средний' { 2 } default { 1 } }
}

function Get-WmmaIdPrefix {
    param([string]$Type)
    switch ($Type) { 'character' { 'CHAR' } 'location' { 'LOC' } 'character_asset' { 'ASSET' } 'scene' { 'SCENE' } { $_ -in @('source','source_note','source_compilation') } { 'SRC' } default { 'LORE' } }
}

function Get-WmmaReceipt {
    param([string]$Root,[string]$Id)
    $path=Join-Path $Root '09_Реестры/Входящие.json'
    if(Test-Path -LiteralPath $path){return @((Read-WmmaJson $path).receipts|Where-Object {$_.request_id -eq $Id})|Select-Object -First 1}
    return $null
}

function Save-WmmaReceipt {
    param([string]$Root,[object]$Receipt)
    $path=Join-Path $Root '09_Реестры/Входящие.json'
    $registry=if(Test-Path -LiteralPath $path){Read-WmmaJson $path}else{[pscustomobject]@{schema_version=1;type='intake_registry';receipts=@()}}
    $registry.receipts=@($registry.receipts|Where-Object {$_.request_id -ne $Receipt.request_id})+@($Receipt)
    Write-WmmaJson $path $registry
}

# Compatibility names used by existing public scripts. Their implementations live here.
function Read-Text {
    param([string]$Path)
    if(-not (Test-Path -LiteralPath $Path)){return ''}
    return Read-WmmaText $Path
}
function Write-Utf8NoBom {
    param([string]$Path,[string]$Text)
    Write-WmmaText $Path $Text
}
function Get-SectionText {
    param([string]$Text,[string]$Heading)
    return Get-WmmaSection $Text $Heading
}
