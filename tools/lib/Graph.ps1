function Get-WmmaDocumentReferences {
    param([string]$Root,[string]$DocumentPath,[string]$Text,[System.Collections.IDictionary]$KnownPaths)
    $tick=[char]96
    $patterns=@(('['+$tick+']([^'+$tick+'\r\n]+\.md)['+$tick+']'),'\[[^\]]*\]\(\s*<?([^<>\r\n]+?\.md)(?:#[^)\s>]*)?>?\s*\)')
    $found=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($pattern in $patterns){foreach($match in [regex]::Matches($Text,$pattern)){
        $ref=[Uri]::UnescapeDataString($match.Groups[1].Value).Replace('\','/')
        if($ref -match '^[a-z]+://' -or [IO.Path]::IsPathRooted($ref)){continue}
        foreach($candidate in @($ref,((Split-Path -Parent $DocumentPath).Replace('\','/')+'/'+$ref))){
            if($null -ne $KnownPaths){
                # The graph already enumerated its documents. Resolve against that
                # inventory instead of walking the filesystem for every mention.
                try{$full=[IO.Path]::GetFullPath((Join-Path $Root $candidate))}catch{continue}
                $base=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
                if(-not $full.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){continue}
                $relative=Get-WmmaRelativePath $Root $full
                if($KnownPaths.Contains($relative)){[void]$found.Add($relative);break}
            }else{
                try{$full=Resolve-WmmaPath $Root $candidate}catch{continue}
                if(Test-Path -LiteralPath $full -PathType Leaf){[void]$found.Add((Get-WmmaRelativePath $Root $full));break}
            }
        }
    }}
    return @($found|Sort-Object)
}

function Get-WmmaHistoryIndex {
    param([string]$Root)
    $result=@{};$path=Join-Path $Root '10_Обслуживание/Миграция_v2.json'
    if(Test-Path -LiteralPath $path){foreach($item in (Read-WmmaJson $path).history){$result[$item.original_path]=@($result[$item.original_path])+@($item.history_path)}}
    return $result
}

function New-WmmaGraph {
    param([string]$Root,[switch]$AssignMissingIds)
    $docs=@(Get-WmmaDocuments $Root|Sort-Object path);$byPath=@{};$byId=@{};$next=@{}
    foreach($doc in $docs){
        if($doc.id){
            if($byId.ContainsKey($doc.id)){throw "Duplicate entity ID: $($doc.id)"}
            $byId[$doc.id]=$doc
            if($doc.id -match '^([A-Z]+)-(\d+)$'){$next[$Matches[1]]=[Math]::Max([int]$next[$Matches[1]],[int]$Matches[2])}
        }
    }
    foreach($doc in $docs){
        if(-not $doc.id){
            if(-not $AssignMissingIds){throw "Missing ID: $($doc.path)"}
            $prefix=Get-WmmaIdPrefix $doc.type;$next[$prefix]=[int]$next[$prefix]+1;$doc.id='{0}-{1:D4}' -f $prefix,$next[$prefix]
            Write-WmmaText $doc.full_path (Set-WmmaMeta $doc.text 'id' $doc.id)
            $doc.text=Read-WmmaText $doc.full_path;$byId[$doc.id]=$doc
        }
        $byPath[$doc.path]=$doc.id
    }
    $history=Get-WmmaHistoryIndex $Root
    $nodes=[Collections.Generic.List[object]]::new();$edges=[Collections.Generic.List[object]]::new()
    $keys=[Collections.Generic.HashSet[string]]::new()
    function Add-GraphEdge([string]$From,[string]$To,[string]$Kind,[string]$Evidence,[string]$Scope='current'){
        if($From -eq $To -and $Kind -in @('references','source_reference')){return}
        $key="$From|$To|$Kind|$Evidence|$Scope"
        if($keys.Add($key)){$edges.Add([ordered]@{from=$From;to=$To;kind=$Kind;evidence=$Evidence;scope=$Scope})}
    }
    foreach($doc in $docs){
        $name=([regex]::Match($doc.text,'(?m)^# (.+)')).Groups[1].Value.Trim()
        $fronts=@(Get-WmmaArrayMeta $doc.text 'front_ids');$legacy=Get-WmmaMeta $doc.text 'front_id'
        if($legacy -and $legacy -ne '-'){$fronts+=@($legacy)}
        $sources=@(Get-WmmaArrayMeta $doc.text 'source_ids');$participants=@(Get-WmmaArrayMeta $doc.text 'participant_ids')
        foreach($id in $sources){Add-GraphEdge $doc.id $id 'sourced_from' $doc.path}
        foreach($id in $participants){Add-GraphEdge $doc.id $id 'participant' $doc.path}
        $documents=@([pscustomobject]@{path=$doc.path;text=$doc.text;scope='current'})
        foreach($hp in @($history[$doc.path]|Where-Object {$_})){
            $documents+=[pscustomobject]@{path=$hp;text=(Read-WmmaText (Join-Path $Root $hp));scope='historical'}
        }
        foreach($item in $documents){foreach($ref in @(Get-WmmaDocumentReferences $Root $item.path $item.text -KnownPaths $byPath)){
            if(-not $byPath.ContainsKey($ref)){continue}
            $target=$byPath[$ref]
            $kind=if($byId[$target].type -like 'source*'){'source_reference'}else{'references'}
            Add-GraphEdge $doc.id $target $kind $item.path $item.scope
        }}
        $nodes.Add([ordered]@{
            id=$doc.id;type=$doc.type;name=$name;path=$doc.path;status=(Get-WmmaMeta $doc.text 'status')
            chapter=(Get-WmmaMeta $doc.text 'chapter');branch=(Get-WmmaMeta $doc.text 'branch')
            aliases=@(@($name)+@(Get-WmmaArrayMeta $doc.text 'aliases')|Sort-Object -Unique)
            front_ids=@($fronts|Sort-Object -Unique);source_ids=$sources;participant_ids=$participants
            history_paths=@($history[$doc.path]|Where-Object {$_});historical_source_ids=@()
            provenance='unresolved';content_sha256=(Get-WmmaHash $doc.text)
        })
    }
    $inboxPath='07_Черновики_и_идеи/Входящие_сообщения.md'
    foreach($entry in @(Get-WmmaInboxEntries (Read-WmmaText (Join-Path $Root $inboxPath)))){
        $sourceRefs=@(Get-WmmaDocumentReferences $Root $inboxPath (Get-WmmaEntryField $entry.Value 'Источник') -KnownPaths $byPath|Where-Object {$byId[$byPath[$_]].type -like 'source*'})
        $sceneRefs=@(Get-WmmaDocumentReferences $Root $inboxPath (Get-WmmaEntryField $entry.Value 'Связано') -KnownPaths $byPath|Where-Object {$byId[$byPath[$_]].type -eq 'scene'})
        foreach($scene in $sceneRefs){foreach($source in $sourceRefs){Add-GraphEdge $byPath[$scene] $byPath[$source] 'sourced_from' $inboxPath}}
    }
    $direct=@{};$past=@{}
    foreach($edge in $edges){if($edge.kind -in @('sourced_from','source_reference')){
        if($edge.scope -eq 'historical'){$past[$edge.from]=@($past[$edge.from])+@($edge.to)}
        else{$direct[$edge.from]=@($direct[$edge.from])+@($edge.to)}
    }}
    foreach($node in $nodes){
        $node.source_ids=@($direct[$node.id]|Where-Object {$_}|Sort-Object -Unique)
        $node.historical_source_ids=@($past[$node.id]|Where-Object {$_}|Sort-Object -Unique)
        $node.provenance=if($node.type -like 'source*'){'original'}elseif($node.source_ids.Count){'linked'}elseif($node.historical_source_ids.Count){'historical'}else{'unresolved'}
    }
    $ordered=@($edges|Sort-Object from,to,kind,evidence,scope)
    return [pscustomobject][ordered]@{
        schema_version=3;type='entity_graph';generated_by='tools/Собрать_связи.ps1'
        entities=@($nodes)
        edges=@($ordered|Where-Object kind -ne 'references')
        references=@($ordered|Where-Object kind -eq 'references')
    }
}
