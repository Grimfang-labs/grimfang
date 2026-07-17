param([int]$IntervalSec = 60, [long]$TargetPositions = 300000000)
$recordSize = 74
$prev = $null
while ($true) {
    $files = Get-ChildItem tools\data\shard_*.bin -ErrorAction SilentlyContinue
    if ($files) {
        $bytes = ($files | Measure-Object Length -Sum).Sum
        $pos = [long]($bytes / $recordSize)
        $now = Get-Date
        if ($prev) {
            $rate = ($pos - $prev.Pos) / ($now - $prev.Time).TotalSeconds
            $eta = if ($rate -gt 0) { [TimeSpan]::FromSeconds(($TargetPositions - $pos) / $rate) } else { $null }
            "{0:HH:mm:ss}  {1,12:N0} pos  ({2,5:N0}/sec)  {3:P1} of target  ETA ~{4:d\.hh\:mm}" -f $now, $pos, $rate, ($pos / $TargetPositions), $eta
        } else {
            "{0:HH:mm:ss}  {1,12:N0} pos  (baseline)" -f $now, $pos
        }
        $prev = @{ Pos = $pos; Time = $now }
    }
    Start-Sleep -Seconds $IntervalSec
}