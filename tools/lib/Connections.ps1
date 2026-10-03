# Only explicit IDs connect registries to documents. This index is transient:
# inferred, reverse and transitive relationships are never written into canon.
function New-WmmaConnectionIndex {
    param([object]$Data,[switch]$IncludeReferences,[switch]$IncludeHistory)
    $nodes=@{};$adjacent=@{};$keys=[Collections.Generic.HashSet[string]]::new()
    foreach($entity in $Data.graph.entities){$nodes[$entity.id]=$entity}
    function Add-Node([string]$Id,[string]$Type,[string]$Name,[string]$Path){
        if($nodes.ContainsKey($Id)){throw "Duplicate connection ID: $Id"}
        $nodes[$Id]=[pscustomobject]@{id=$Id;type=$Type;name=$Name;path=$Path}
    }
    function Add-Connection([string]$From,[string]$To,[string]$Kind,[string]$Evidence,[string]$Scope='current'){
        if(-not $From -or -not $To -or $From -eq $To){return}
        if(-not $IncludeHistory -and $Scope -eq 'historical'){return}
        if(-not $IncludeReferences -and $Kind -eq 'references'){return}
        if(-not $nodes.ContainsKey($From) -or -not $nodes.ContainsKey($To)){throw "Unknown connection endpoint: $From -> $To"}
        if(-not $keys.Add("$From|$To|$Kind|$Evidence|$Scope")){return}
        $edge=[pscustomobject]@{from=$From;to=$To;kind=$Kind;evidence=$Evidence;scope=$Scope}
        Add-WmmaIndexItem $adjacent $From ([pscustomobject]@{id=$To;direction='outgoing';edge=$edge})
        Add-WmmaIndexItem $adjacent $To ([pscustomobject]@{id=$From;direction='incoming';edge=$edge})
    }
    foreach($fact in $Data.knowledge.facts){Add-Node $fact.id 'fact' $fact.text '09_Реестры/Знания.json'}
    foreach($decision in $Data.decisions.decisions){
        $name=if($decision.state -eq 'pending'){$decision.question}else{$decision.choice}
        Add-Node $decision.id 'decision' $name '09_Реестры/Решения.json'
    }
    foreach($question in $Data.questions.questions){Add-Node $question.id 'question' $question.text '09_Реестры/Вопросы.json'}
    foreach($front in $Data.fronts.fronts){Add-Node $front.id 'front' $front.name '09_Реестры/Фронты.json'}
    foreach($edge in @($Data.graph.edges)+@($Data.graph.references)){
        if($edge){Add-Connection $edge.from $edge.to $edge.kind $edge.evidence $edge.scope}
    }
    foreach($entity in $Data.graph.entities){
        foreach($id in $entity.front_ids){Add-Connection $entity.id $id 'front' $entity.path}
    }
    foreach($fact in $Data.knowledge.facts){
        foreach($id in $fact.subject_ids){Add-Connection $fact.id $id 'subject' '09_Реестры/Знания.json'}
        foreach($id in $fact.evidence_ids){Add-Connection $fact.id $id 'evidence' '09_Реестры/Знания.json'}
        foreach($id in $fact.known_to){
            if(Test-WmmaFactVisible $fact $id){Add-Connection $fact.id $id 'known_by' '09_Реестры/Знания.json'}
        }
    }
    foreach($decision in $Data.decisions.decisions){
        foreach($id in $decision.scene_ids){Add-Connection $decision.id $id 'scene' '09_Реестры/Решения.json'}
        if($IncludeReferences){foreach($path in $decision.link_paths){
            if($Data.entities_by_path.ContainsKey($path)){
                Add-Connection $decision.id $Data.entities_by_path[$path].id 'references' '09_Реестры/Решения.json'
            }
        }}
    }
    foreach($branch in $Data.state.branches){
        foreach($id in $branch.scene_ids){Add-Connection $branch.character_id $id 'branch_scene' '09_Реестры/Контекст.json'}
        # A focus is an editor's work queue, not knowledge possessed by a character.
        foreach($id in $branch.focus_ids){
            $target=if($Data.decision_aliases.ContainsKey($id)){$Data.decision_aliases[$id].id}else{$id}
            Add-Connection $branch.character_id $target 'branch_focus' '09_Реестры/Контекст.json'
        }
    }
    return [pscustomobject]@{nodes=$nodes;adjacent=$adjacent}
}

function Get-WmmaConnections {
    param(
        [string]$Root,[Parameter(Mandatory)][string]$Entity,
        [ValidateRange(1,3)][int]$Depth=1,[ValidateRange(1,200)][int]$MaxNodes=20,
        [switch]$IncludeReferences,[switch]$IncludeHistory,[object]$Data
    )
    $Data=Resolve-WmmaReadModel $Root $Data
    $index=New-WmmaConnectionIndex $Data -IncludeReferences:$IncludeReferences -IncludeHistory:$IncludeHistory
    $requested=$Entity
    if($Data.decision_aliases.ContainsKey($Entity)){$Entity=$Data.decision_aliases[$Entity].id}
    $start=if($index.nodes.ContainsKey($Entity)){$index.nodes[$Entity]}else{Resolve-WmmaEntity $Data $Entity}
    $visited=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$visited.Add($start.id)
    $queue=[Collections.Generic.Queue[object]]::new()
    $queue.Enqueue([pscustomobject]@{id=$start.id;distance=0})
    $result=[Collections.Generic.List[object]]::new();$truncated=$false
    while($queue.Count -and -not $truncated){
        $current=$queue.Dequeue()
        if($current.distance -ge $Depth){continue}
        $groups=@($index.adjacent[$current.id]|Where-Object {$_}|Group-Object id|Sort-Object Name)
        foreach($group in $groups){
            if($visited.Contains($group.Name)){continue}
            if($result.Count -ge $MaxNodes){$truncated=$true;break}
            [void]$visited.Add($group.Name)
            $node=$index.nodes[$group.Name]
            $proofs=@($group.Group|Sort-Object direction,@{Expression={$_.edge.kind}},@{Expression={$_.edge.evidence}},@{Expression={$_.edge.scope}}|ForEach-Object {
                [pscustomobject]@{direction=$_.direction;kind=$_.edge.kind;evidence=$_.edge.evidence;scope=$_.edge.scope}
            })
            $result.Add([pscustomobject]@{
                id=$node.id;type=$node.type;name=$node.name;path=$node.path
                distance=($current.distance+1);via=$current.id;relations=$proofs
            })
            $queue.Enqueue([pscustomobject]@{id=$node.id;distance=($current.distance+1)})
        }
    }
    return [pscustomobject]@{
        requested_id=$requested;entity=[pscustomobject]@{id=$start.id;name=$start.name;type=$start.type;path=$start.path}
        audience='gm';depth=$Depth;max_nodes=$MaxNodes
        include_references=[bool]$IncludeReferences;include_history=[bool]$IncludeHistory
        connections=@($result);truncated=$truncated
        note='Каждый узел показан один раз по кратчайшему найденному маршруту. Связь не подтверждает истинность текста, присутствие или знание персонажа.'
    }
}

# Iterative DFS visits each ordering node once, including converging paths.
# Overlaps is not an ordering relation. Cycles in ordinary mentions are allowed.
function Get-WmmaOrderCycles {
    param([object[]]$Relations)
    $next=@{};$state=@{};$position=@{}
    foreach($relation in $Relations){
        if($relation.relation -notin @('after','learned_after')){continue}
        if(-not $next.ContainsKey($relation.from)){$next[$relation.from]=[Collections.Generic.HashSet[string]]::new()}
        if(-not $next.ContainsKey($relation.to)){$next[$relation.to]=[Collections.Generic.HashSet[string]]::new()}
        [void]$next[$relation.from].Add($relation.to)
    }
    $stack=[Collections.Generic.Stack[object]]::new();$trail=[Collections.Generic.List[string]]::new()
    foreach($start in @($next.Keys|Sort-Object)){
        if($state[$start]){continue}
        $stack.Push([pscustomobject]@{id=$start;index=0;children=@($next[$start]|Sort-Object)})
        $state[$start]=1;$position[$start]=0;$trail.Add($start)
        while($stack.Count){
            $frame=$stack.Peek()
            if($frame.index -ge $frame.children.Count){
                [void]$stack.Pop();$state[$frame.id]=2;$position.Remove($frame.id);$trail.RemoveAt($trail.Count-1)
                continue
            }
            $child=$frame.children[$frame.index];$frame.index++
            if($state[$child] -eq 1){
                (@($trail.GetRange($position[$child],$trail.Count-$position[$child]))+@($child)) -join ' -> '
            }elseif(-not $state[$child]){
                $state[$child]=1;$position[$child]=$trail.Count;$trail.Add($child)
                $stack.Push([pscustomobject]@{id=$child;index=0;children=@($next[$child]|Sort-Object)})
            }
        }
    }
}
