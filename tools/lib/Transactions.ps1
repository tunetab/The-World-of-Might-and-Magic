function Get-WmmaFileInventory {
    param([string]$Root)
    $result=@{}
    $folders=[Collections.Generic.Stack[string]]::new();$folders.Push($Root)
    while($folders.Count){foreach($file in Get-ChildItem -LiteralPath $folders.Pop() -Force){
        $relative=Get-WmmaRelativePath $Root $file.FullName
        if($relative -match '^(\.git|\.wmma|Все_MD_файлы)(/|$)' -or $relative -match '(^|/)desktop\.ini$|\.(tmp|bak|orig)$'){continue}
        if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse point cannot be copied into a turn: $relative"}
        if($file.PSIsContainer){$folders.Push($file.FullName);continue}
        $result[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }}
    return $result
}

function Get-WmmaTurnPath {
    param([string]$Root,[string]$Id)
    if($Id -notmatch '^TURN-[a-zA-Z0-9-]+$'){throw 'Invalid turn ID.'}
    return Resolve-WmmaPath $Root ('.wmma/turns/'+$Id)
}

function Start-WmmaTurn {
    param([string]$Root,[string]$Id,[string]$Title)
    $turn=Get-WmmaTurnPath $Root $Id
    $manifestPath=Join-Path $turn 'transaction.json'
    $manifest=if(Test-Path -LiteralPath $manifestPath){Read-WmmaJson $manifestPath}else{$null}
    if($manifest -and $manifest.title -cne $Title){throw 'Turn ID already belongs to another title.'}
    if($manifest -and $manifest.status -ne 'copying'){return $manifest}
    $stage=Join-Path $turn 'stage'
    [IO.Directory]::CreateDirectory($stage)|Out-Null
    $inventory=Get-WmmaFileInventory $Root
    if(-not $manifest){
        $manifest=[pscustomobject][ordered]@{schema_version=2;id=$Id;title=$Title;status='copying';base=$inventory;changes=@();applied=@();prepared_digest=$null}
        Write-WmmaJson $manifestPath $manifest
    }else{
        foreach($p in $manifest.base.PSObject.Properties){if($inventory[$p.Name] -cne $p.Value){throw "Source changed during interrupted snapshot: $($p.Name)"}}
        if($inventory.Count -ne @($manifest.base.PSObject.Properties).Count){throw 'File list changed during interrupted snapshot.'}
    }
    foreach($path in $inventory.Keys){
        $target=Resolve-WmmaPath $stage $path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
        if(Test-Path -LiteralPath $target){if((Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() -cne $inventory[$path]){throw "Incomplete snapshot contains a changed file: $path"};continue}
        [IO.File]::Copy((Resolve-WmmaPath $Root $path),$target)
    }
    $afterCopy=Get-WmmaFileInventory $Root
    foreach($key in $inventory.Keys){if($afterCopy[$key] -cne $inventory[$key]){throw "Source changed while copying: $key"}}
    if($afterCopy.Count -ne $inventory.Count){throw 'Source file list changed while copying.'}
    $manifest.status='editing'
    Write-WmmaJson $manifestPath $manifest
    return $manifest
}

function Invoke-WmmaStageValidation {
    param([string]$Stage,[switch]$Build)
    $program=if($Build){'Завершить_ход.ps1'}else{'Проверить_проект.ps1'}
    $shell=(Get-Process -Id $PID).Path
    $log=Join-Path $Stage '.wmma/validation.log';[IO.Directory]::CreateDirectory((Split-Path -Parent $log))|Out-Null
    & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Stage ('tools/'+$program)) *> $log
    if($LASTEXITCODE -ne 0){$details=(Get-Content -LiteralPath $log -Tail 45)-join [Environment]::NewLine;throw "Stage validation failed: $program. Log: $log$([Environment]::NewLine)$details"}
}

function Get-WmmaPlanDigest {
    param([object]$Manifest)
    if($Manifest.schema_version -lt 2){return Get-WmmaHash (ConvertTo-Json -InputObject @($Manifest.changes) -Depth 10 -Compress)}
    $base=@($Manifest.base.PSObject.Properties|Sort-Object Name|ForEach-Object {[ordered]@{path=$_.Name;hash=$_.Value}})
    return Get-WmmaHash (ConvertTo-Json -InputObject ([ordered]@{id=$Manifest.id;base=$base;changes=@($Manifest.changes)}) -Depth 20 -Compress)
}

function Assert-WmmaTurnPlan {
    param([string]$Root,[object]$Manifest,[string]$Id)
    if($Manifest.id -cne $Id -or $Manifest.schema_version -notin @(1,2)){throw 'Invalid turn identity or schema.'}
    if($Manifest.prepared_digest -cne (Get-WmmaPlanDigest $Manifest)){throw 'Prepared plan was modified.'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($change in $Manifest.changes){
        $full=Resolve-WmmaPath $Root $change.path
        $relative=Get-WmmaRelativePath $Root $full
        if($relative -match '^(\.git|\.wmma)(/|$)' -or $relative -cne $change.path.Replace('\','/')){throw 'Noncanonical or protected turn path.'}
        if(-not $paths.Add($relative)){throw 'Duplicate path in prepared turn.'}
        foreach($hash in @($change.before,$change.after)|Where-Object {$_}){if($hash -notmatch '^[a-f0-9]{64}$'){throw 'Invalid hash in prepared turn.'}}
        if($change.before -ceq $change.after){throw 'Turn contains an unchanged operation.'}
    }
}

function Prepare-WmmaTurn {
    param([string]$Root,[string]$Id)
    $turn=Get-WmmaTurnPath $Root $Id
    $path=Join-Path $turn 'transaction.json';$m=Read-WmmaJson $path
    if($m.status -notin @('editing','prepared','restored')){throw "Cannot prepare a $($m.status) turn."}
    $stage=Join-Path $turn 'stage'
    Invoke-WmmaStageValidation $stage -Build
    $after=Get-WmmaFileInventory $stage
    $base=@{};foreach($p in $m.base.PSObject.Properties){$base[$p.Name]=$p.Value}
    $changes=@()
    foreach($p in @(@($base.Keys)+@($after.Keys)|Sort-Object -Unique)){
        if($base[$p] -ceq $after[$p]){continue}
        if($p -match '^(\.git|\.wmma)(/|$)'){throw 'Local runtime paths cannot be in a turn.'}
        $changes += [pscustomobject]@{path=$p;before=$base[$p];after=$after[$p]}
    }
    $m.changes=$changes;$m.applied=@();$m.status='prepared'
    $m.schema_version=2
    $m.prepared_digest=Get-WmmaPlanDigest $m
    Write-WmmaJson $path $m
    return $m
}

function Apply-WmmaTurn {
    param([string]$Root,[string]$Id,[int]$FailAfter=0)
    $turn=Get-WmmaTurnPath $Root $Id
    $manifestPath=Join-Path $turn 'transaction.json';$m=Read-WmmaJson $manifestPath
    if($m.status -eq 'committed'){Assert-WmmaTurnPlan $Root $m $Id;return $m}
    if($m.status -notin @('prepared','applying')){throw "Turn is not prepared: $($m.status)"}
    Assert-WmmaTurnPlan $Root $m $Id
    $stage=Join-Path $turn 'stage'
    $current=Get-WmmaFileInventory $Root
    $staged=Get-WmmaFileInventory $stage
    # Verify every operation before the first write. On recovery each target may be
    # at its before or after hash; unrelated concurrent edits are never overwritten.
    foreach($change in $m.changes){
        [void](Resolve-WmmaPath $Root $change.path)
        if($change.path -match '^(\.git|\.wmma)(/|$)'){throw 'Protected runtime path.'}
        if($staged[$change.path] -cne $change.after){throw "Stage changed after preparation: $($change.path)"}
        if($m.status -eq 'prepared' -and $current[$change.path] -cne $change.before){throw "Concurrent edit before application: $($change.path)"}
        if($current[$change.path] -cne $change.before -and $current[$change.path] -cne $change.after){throw "Concurrent edit: $($change.path)"}
    }
    # Dependencies not changed by the plan must also match the base. Otherwise
    # staged validation would have validated a different campaign state.
    $changed=@($m.changes|ForEach-Object {$_.path})
    foreach($p in $m.base.PSObject.Properties){
        if($changed -notcontains $p.Name -and $current[$p.Name] -cne $p.Value){throw "Changed dependency: $($p.Name)"}
        if($changed -notcontains $p.Name -and $staged[$p.Name] -cne $p.Value){throw "Stage dependency changed after preparation: $($p.Name)"}
    }
    foreach($p in $current.Keys){if(-not $m.base.PSObject.Properties[$p] -and $changed -notcontains $p){throw "New concurrent file: $p"}}
    foreach($p in $staged.Keys){if(-not $m.base.PSObject.Properties[$p] -and $changed -notcontains $p){throw "New stage file after preparation: $p"}}
    Invoke-WmmaStageValidation $stage
    $m.status='applying';Write-WmmaJson $manifestPath $m
    $count=0
    foreach($change in $m.changes){
        if($current[$change.path] -ceq $change.after){
            if($change.before){$backup=Resolve-WmmaPath (Join-Path $turn 'backup') $change.path;if(-not (Test-Path -LiteralPath $backup) -or (Get-FileHash -LiteralPath $backup).Hash.ToLowerInvariant() -cne $change.before){throw 'Recovery backup is missing or invalid.'}}
            continue
        }
        $target=Resolve-WmmaPath $Root $change.path
        $targetHash=if(Test-Path -LiteralPath $target){(Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant()}else{$null}
        if($targetHash -cne $change.before){throw "Concurrent edit during application: $($change.path)"}
        $backup=Resolve-WmmaPath (Join-Path $turn 'backup') $change.path
        if($change.before -and -not (Test-Path -LiteralPath $backup)){
            [IO.Directory]::CreateDirectory((Split-Path -Parent $backup))|Out-Null
            [IO.File]::Copy($target,$backup)
        }
        if($change.before -and (Get-FileHash -LiteralPath $backup).Hash.ToLowerInvariant() -cne $change.before){throw "Invalid recovery backup: $($change.path)"}
        if($change.after){
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
            $temp=$target+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
            [IO.File]::Copy((Resolve-WmmaPath $stage $change.path),$temp)
            if((Get-FileHash -LiteralPath $temp).Hash.ToLowerInvariant() -cne $change.after){throw "Staged payload changed during application: $($change.path)"}
            if(Test-Path -LiteralPath $target){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
        }else{[IO.File]::Delete($target)}
        $m.applied=@($m.applied)+@($change.path);Write-WmmaJson $manifestPath $m
        $count++
        if($FailAfter -gt 0 -and $count -ge $FailAfter){throw 'Simulated interruption; journal retained for recovery.'}
    }
    $final=Get-WmmaFileInventory $Root
    if($final.Count -ne $staged.Count){throw 'File list changed during application; journal retained.'}
    foreach($path in $staged.Keys){if($final[$path] -cne $staged[$path]){throw "Project changed during application: $path; journal retained."}}
    $m.status='committed';Write-WmmaJson $manifestPath $m
    return $m
}

function Undo-WmmaTurn {
    param([string]$Root,[string]$Id)
    $turn=Get-WmmaTurnPath $Root $Id;$mp=Join-Path $turn 'transaction.json';$m=Read-WmmaJson $mp
    Assert-WmmaTurnPlan $Root $m $Id
    if($m.status -eq 'restored'){return $m}
    if($m.status -notin @('applying','committed','restoring')){throw 'No applied turn to restore.'}
    $current=Get-WmmaFileInventory $Root
    foreach($change in $m.changes){
        if($current[$change.path] -cne $change.before -and $current[$change.path] -cne $change.after){throw "Concurrent edit prevents restoration: $($change.path)"}
        if($change.before -and $current[$change.path] -cne $change.before){
            $backup=Resolve-WmmaPath (Join-Path $turn 'backup') $change.path
            if(-not (Test-Path -LiteralPath $backup) -or (Get-FileHash -LiteralPath $backup).Hash.ToLowerInvariant() -cne $change.before){throw "Missing or invalid backup: $($change.path)"}
        }
    }
    $m.status='restoring';Write-WmmaJson $mp $m
    foreach($change in $m.changes){
        if($current[$change.path] -ceq $change.before){continue}
        $target=Resolve-WmmaPath $Root $change.path
        $actual=if(Test-Path -LiteralPath $target){(Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant()}else{$null}
        if($actual -cne $change.after){throw "Concurrent edit during restoration: $($change.path)"}
        if($change.before){
            $backup=Resolve-WmmaPath (Join-Path $turn 'backup') $change.path
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
            $temp=$target+'.'+[guid]::NewGuid().ToString('N')+'.tmp';[IO.File]::Copy($backup,$temp)
            if((Get-FileHash -LiteralPath $temp).Hash.ToLowerInvariant() -cne $change.before){throw "Backup changed during restoration: $($change.path)"}
            if(Test-Path -LiteralPath $target){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
        }else{[IO.File]::Delete($target)}
    }
    $m.status='restored';Write-WmmaJson $mp $m;return $m
}
