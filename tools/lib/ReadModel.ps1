# One read-only snapshot per operation. Pass it explicitly to cooperating readers;
# never reuse it after a write or keep it in process-global state.
function Add-WmmaIndexItem {
    param([hashtable]$Index,[string]$Key,[object]$Value)
    if(-not $Key){return}
    if(-not $Index.ContainsKey($Key)){$Index[$Key]=[Collections.Generic.List[object]]::new()}
    $Index[$Key].Add($Value)
}

function New-WmmaIdIndex {
    param([object[]]$Items)
    $index=@{}
    foreach($item in $Items){
        if(-not $item.id){throw 'An indexed record needs an ID.'}
        if($index.ContainsKey($item.id)){throw "Duplicate record ID: $($item.id)"}
        $index[$item.id]=$item
    }
    return $index
}

function Test-WmmaFactVisible {
    param([object]$Fact,[string]$Viewer,[ValidateSet('gm','character')][string]$Audience='character')
    return $Audience -eq 'gm' -or ($Fact.visibility -ne 'gm' -and (
        $Fact.visibility -eq 'public' -or ($Viewer -and $Fact.known_to -contains $Viewer)))
}

function New-WmmaReadModel {
    param([string]$Root)
    $resolved=(Resolve-Path -LiteralPath $Root).Path
    $registries=@{}
    foreach($name in @('Сущности','Контекст','Знания','Решения','Вопросы','Фронты')){
        $registries[$name]=Read-WmmaJson (Join-Path $resolved "09_Реестры/$name.json")
    }
    $graph=$registries['Сущности'];$knowledge=$registries['Знания']
    $byId=New-WmmaIdIndex @($graph.entities)
    $byPath=@{};$byAlias=@{};$byType=@{};$scenesByParticipant=@{};$factsBySubject=@{}
    foreach($entity in $graph.entities){
        $byPath[$entity.path]=$entity
        Add-WmmaIndexItem $byType $entity.type $entity
        foreach($alias in @(@($entity.name)+@($entity.aliases)|Sort-Object -Unique)){
            Add-WmmaIndexItem $byAlias $alias $entity
        }
        if($entity.type -eq 'scene'){
            foreach($id in @($entity.participant_ids|Sort-Object -Unique)){
                Add-WmmaIndexItem $scenesByParticipant $id $entity
            }
        }
    }
    foreach($fact in $knowledge.facts){
        foreach($id in @($fact.subject_ids|Sort-Object -Unique)){Add-WmmaIndexItem $factsBySubject $id $fact}
    }
    $decisions=New-WmmaIdIndex @($registries['Решения'].decisions)
    $decisionAliases=@{}
    foreach($decision in $registries['Решения'].decisions){
        if($decision.resolved_from){
            if($decisions.ContainsKey($decision.resolved_from) -or $decisionAliases.ContainsKey($decision.resolved_from)){
                throw "Ambiguous decision transition: $($decision.resolved_from)"
            }
            $decisionAliases[$decision.resolved_from]=$decision
        }
    }
    return [pscustomobject]@{
        root=$resolved;chapter=(Get-WmmaCurrentChapter $resolved)
        graph=$graph;state=$registries['Контекст'];knowledge=$knowledge
        decisions=$registries['Решения'];questions=$registries['Вопросы'];fronts=$registries['Фронты']
        entities_by_id=$byId;entities_by_path=$byPath;entities_by_alias=$byAlias;entities_by_type=$byType
        scenes_by_participant=$scenesByParticipant;facts_by_subject=$factsBySubject
        facts_by_id=(New-WmmaIdIndex @($knowledge.facts))
        decisions_by_id=$decisions;decision_aliases=$decisionAliases
        questions_by_id=(New-WmmaIdIndex @($registries['Вопросы'].questions))
        fronts_by_id=(New-WmmaIdIndex @($registries['Фронты'].fronts))
        history_index=(Get-WmmaHistoryIndex $resolved)
    }
}

function Resolve-WmmaReadModel {
    param([string]$Root,[object]$Data)
    if($null -eq $Data){return New-WmmaReadModel $Root}
    if((Resolve-Path -LiteralPath $Root).Path -ne $Data.root){throw 'Read model belongs to another project.'}
    return $Data
}

function Resolve-WmmaEntity {
    param([object]$Data,[string]$Entity)
    if($Data.entities_by_id.ContainsKey($Entity)){return $Data.entities_by_id[$Entity]}
    $matches=@($Data.entities_by_alias[$Entity]|Where-Object {$_})
    if($matches.Count -ne 1){throw "Entity is missing or ambiguous: $Entity ($($matches.Count) matches). Use a stable ID."}
    return $matches[0]
}

function Resolve-WmmaBranch {
    param([object]$Data,[string]$Branch)
    if(-not $Branch){return $null}
    $matches=@($Data.state.branches|Where-Object {$_.name -eq $Branch -or $_.character_id -eq $Branch})
    if($matches.Count -ne 1){throw "Unknown or ambiguous branch: $Branch"}
    return $matches[0]
}
