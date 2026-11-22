# Uruchamia NaszOS w QEMU bez okna, wysyla klawisze i robi zrzut ekranu PNG
param([string]$Disk, [string]$Res = '', [string[]]$Keys = @(), [string]$Png, [int]$Wait = 4, [string]$Extra = '')
Stop-Process -Name qemu-system-x86_64 -Force -ErrorAction SilentlyContinue
$qargs = "-kernel C:\Users\relax\windows_exe\build\kernel.elf -display none -rtc base=localtime -monitor tcp:127.0.0.1:4560,server,nowait -drive `"file=$Disk,format=raw,if=ide,index=0`""
if ($Extra) { $qargs += " $Extra" }
if ($Res) { $qargs += " -append `"res=$Res`"" }
$p = Start-Process 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList $qargs -PassThru -WindowStyle Hidden
Start-Sleep $Wait
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4560); $s = $c.GetStream()
$w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
$junk = New-Object byte[] 65536
function drain { while ($s.DataAvailable) { [void]$s.Read($junk, 0, $junk.Length) } }
# Mysz startuje na srodku ekranu; "at:X,Y" przesuwa ja do punktu, "click"/"dbl" klika,
# "mdown"/"mup" wciska/puszcza lewy przycisk (do przeciagania)
$mx = 960; $my = 600
if ($Res -match '^(\d+)x(\d+)$') { $mx = [int]$Matches[1] / 2; $my = [int]$Matches[2] / 2 }
foreach ($k in $Keys) {
    if ($k -match '^sleep(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    if ($k -match '^at:(\d+),(\d+)$') {
        $tx = [int]$Matches[1]; $ty = [int]$Matches[2]
        while ($mx -ne $tx -or $my -ne $ty) {       # kroki po max 100 px (pakiet PS/2 ma limit)
            $dx = [math]::Max(-100, [math]::Min(100, $tx - $mx)); $dy = [math]::Max(-100, [math]::Min(100, $ty - $my))
            $w.WriteLine("mouse_move $dx $dy"); Start-Sleep -Milliseconds 30; drain
            $mx += $dx; $my += $dy
        }
        Start-Sleep -Milliseconds 100; continue
    }
    if ($k -match '^wheel:(-?\d+)$') { $w.WriteLine("mouse_move 0 0 $($Matches[1])"); Start-Sleep -Milliseconds 150; drain; continue }
    if ($k -eq 'click') { $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 60; $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 200; drain; continue }
    if ($k -eq 'dbl') { foreach ($n in 1,2) { $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 50; $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 80 }; Start-Sleep -Milliseconds 300; drain; continue }
    if ($k -eq 'mdown') { $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 100; drain; continue }
    if ($k -eq 'mup') { $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 100; drain; continue }
    $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 80; drain
}
Start-Sleep -Milliseconds 500
Remove-Item $Png -ErrorAction SilentlyContinue
$w.WriteLine("screendump $($Png -replace '\\','/') -f png"); Start-Sleep 2; drain
$w.WriteLine('quit'); Start-Sleep 1; $c.Close()
if (Test-Path $Png) { "Zrzut: $Png ($((Get-Item $Png).Length) B)" } else { "BRAK ZRZUTU" }
