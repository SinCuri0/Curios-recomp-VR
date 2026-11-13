param([Parameter(Mandatory)][string]$Log)

while (-not (Test-Path $Log)) { Start-Sleep -Milliseconds 500 }
$start = Get-Date
# break inside ForEach-Object leaves the enclosing loop: this do-loop, not the caller's script.
do {
    Get-Content $Log -Wait -Tail 2000 | ForEach-Object {
        if ($_ -match '^\[(\d+)/(\d+)\]') {
            $done = [int]$Matches[1]; $all = [int]$Matches[2]
            $pct = [math]::Floor(100 * $done / [math]::Max($all, 1))
            $bar = ('#' * [math]::Floor($pct / 4)).PadRight(25, '.')
            $mins = [math]::Floor(((Get-Date) - $start).TotalMinutes)
            Write-Host -NoNewline ("`r[{0}] {1,3}%  {2}/{3} files  {4} min " -f $bar, $pct, $done, $all, $mins)
        } elseif ($_ -match '^FAILED|\): error|: error:|fatal error') {
            Write-Host ''
            Write-Host $_ -ForegroundColor Red
        } elseif ($_ -match '^BUILD DONE') {
            Write-Host ''
            Write-Host $_ -ForegroundColor $(if ($_ -match 'exit 0') { 'Green' } else { 'Red' })
            break
        } elseif ($_ -match '^ninja: build stopped') {
            Write-Host ''
            Write-Host $_ -ForegroundColor Yellow
        }
    }
} while ($false)
