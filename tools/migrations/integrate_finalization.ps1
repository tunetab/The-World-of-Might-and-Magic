$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$names=@('Новая_сцена','Новая_локация','Новый_персонаж','Новый_портрет','Новый_вопрос','Новое_решение','Новый_фронт','Закрыть_решение','Закрыть_вопрос','Обновить_фронт','Принять_сообщение','Обработать_входящее')
foreach($name in $names){
    $path=Join-Path $root ('tools/'+$name+'.ps1')
    $text=[IO.File]::ReadAllText($path)
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseInput($text,[ref]$tokens,[ref]$errors)
    $blocks=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -eq '-not $SkipCheck'},$true)|Sort-Object {$_.Extent.StartOffset} -Descending)
    foreach($block in $blocks){
        $replacement=@'
if (-not $SkipCheck) {
    & (Join-Path $root 'tools/Завершить_ход.ps1')
    if($LASTEXITCODE -ne 0){throw 'Final turn validation failed.'}
}
'@
        $text=$text.Remove($block.Extent.StartOffset,$block.Extent.EndOffset-$block.Extent.StartOffset).Insert($block.Extent.StartOffset,$replacement)
    }
    [IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))
}
