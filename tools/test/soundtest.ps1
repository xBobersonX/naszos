# Nagrywa dzwiek z glosniczka PC do pliku WAV (QEMU -audiodev wav) i wysyla klawisze
param([string]$Disk, [string]$Wav, [string[]]$Keys = @(), [int]$Wait = 4, [int]$After = 2)
Stop-Process -Name qemu-system-x86_64 -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
$w32 = $Wav -replace '\\', '/'
$kernel = Join-Path $PSScriptRoot '..\..\build\kernel.elf'
$qargs = "-kernel `"$kernel`" -display none -rtc base=localtime " +
         "-audiodev wav,id=snd,path=$w32 -machine pc,pcspk-audiodev=snd -device AC97,audiodev=snd -m 1G -netdev user,id=n0 -device rtl8139,netdev=n0 " +
         "-monitor tcp:127.0.0.1:4563,server,nowait -drive `"file=$Disk,format=raw,if=ide,index=0`""
$p = Start-Process 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList $qargs -PassThru -WindowStyle Hidden -RedirectStandardError "$PSScriptRoot\sound.err"
Start-Sleep $Wait
if ($p.HasExited) { "QEMU zakonczyl sie: $(Get-Content "$PSScriptRoot\sound.err" -Raw)"; exit 1 }
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4563); $w = New-Object IO.StreamWriter($c.GetStream()); $w.AutoFlush = $true
foreach ($k in $Keys) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 80 }
Start-Sleep $After
$w.WriteLine('quit'); Start-Sleep 1; $c.Close()
"WAV: $((Get-Item $Wav).Length) B"
