# Uruchamia kernel w QEMU bez okna i wypisuje zawartosc ekranu VGA
param([string]$Kernel = 'C:\Users\relax\windows_exe\build\kernel.elf', [int]$Wait = 3, [string[]]$Keys = @(), [string]$Disk = '', [string]$Extra = '')
Stop-Process -Name qemu-system-x86_64 -Force -ErrorAction SilentlyContinue
$out = "$env:TEMP\vga.bin"; Remove-Item $out -ErrorAction SilentlyContinue
$qargs = "-kernel `"$Kernel`" -display none -rtc base=localtime -monitor tcp:127.0.0.1:4555,server,nowait"
if ($Extra) { $qargs += " $Extra" }
if ($Disk) { $qargs += " -drive `"file=$Disk,format=raw,if=ide,index=0`"" }
$p = Start-Process 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList $qargs -PassThru -WindowStyle Hidden
Start-Sleep $Wait
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4555); $w = New-Object IO.StreamWriter($c.GetStream()); $w.AutoFlush = $true
foreach ($k in $Keys) {
    if ($k -match '^sleep(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 80
}
Start-Sleep -Milliseconds 300
$w.WriteLine("pmemsave 0xb8000 4000 `"$($out -replace '\\','/')`""); Start-Sleep 1; $w.WriteLine('quit'); Start-Sleep 1; $c.Close()
$b = [IO.File]::ReadAllBytes($out)
$t = -join $(for ($i = 0; $i -lt $b.Length; $i += 2) { [char]$b[$i] })
for ($r = 0; $r -lt 25; $r++) { $l = $t.Substring($r*80, 80).TrimEnd(); if ($l) { $l } }
