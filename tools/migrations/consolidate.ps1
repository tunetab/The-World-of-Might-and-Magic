$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
foreach($file in Get-ChildItem -LiteralPath (Join-Path $root 'tools') -File -Filter '*.ps1'){
    if($file.Name -eq '_lib.ps1'){continue}
    $text=[IO.File]::ReadAllText($file.FullName)
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseInput($text,[ref]$tokens,[ref]$errors)
    $targets=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Write-Utf8NoBom','Read-Text','Get-SectionText')},$true)|Sort-Object {$_.Extent.StartOffset} -Descending)
    foreach($target in $targets){$text=$text.Remove($target.Extent.StartOffset,$target.Extent.EndOffset-$target.Extent.StartOffset)}
    # Skip private local transaction workspaces during all recursive validation.
    $text=$text.Replace("$"+"_.FullName -notmatch '\\.git\\'","$"+"_.FullName -notmatch '\\.git\\'")
    if($file.Name -in @('Проверить_проект.ps1','Проверить_портреты.ps1','Проверить_архив.ps1')){
        $text=$text.Replace("'\\.git\\'","'\\(?:.git|.wmma)\\'")
        $text=$text.Replace("'\\.git\\'","'\\(?:.git|.wmma)\\'")
    }
    # Builder execution does not change the date of underlying canon data.
    if($file.Name -in @('Собрать_решения.ps1','Собрать_вопросы.ps1','Собрать_фронты.ps1')){
        $text=$text.Replace('$registry.updated_real_date = $today','')
    }
    [IO.File]::WriteAllText($file.FullName,$text,[Text.UTF8Encoding]::new($false))
}
