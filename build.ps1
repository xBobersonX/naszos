# Buduje kernel (64-bitowy, x86-64): build\kernel.elf
# Uzycie:  .\build.ps1             - kompilacja
#          .\build.ps1 -Run        - kompilacja + uruchomienie w QEMU na pelnym ekranie
#          .\build.ps1 -Run -Okno  - to samo, ale w oknie 1280x800
param([switch]$Run, [switch]$Okno)

$ErrorActionPreference = 'Stop'

function Find-Tool($name, $fallbackDir) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $path = Join-Path $fallbackDir "$name.exe"
    if (Test-Path $path) { return $path }
    throw "Nie znaleziono '$name'. Zainstaluj go (patrz README.md)."
}

$clang = Find-Tool 'clang'   'C:\Program Files\LLVM\bin'
$lld   = Find-Tool 'ld.lld'  'C:\Program Files\LLVM\bin'
$objcopy = Find-Tool 'llvm-objcopy' 'C:\Program Files\LLVM\bin'

# Wspolne flagi: x86-64 bez systemu pod spodem, bez SSE/FPU (kernel nie
# zapisuje tych rejestrow przy przelaczaniu zadan)
$common = @(
    '--target=x86_64-unknown-none-elf', '-ffreestanding', '-fno-builtin',
    '-fno-stack-protector', '-fno-pic', '-mno-sse', '-mno-mmx', '-mno-80387',
    '-nostdlib', '-std=c11', '-O2', '-Wall', '-Wextra', '-c'
)
# Kernel: -mno-red-zone - przerwanie w kernelu odklada dane tuz pod rsp,
# wiec funkcje nie moga trzymac niczego ponizej wskaznika stosu.
# -mcmodel=small: caly kernel lezy w pierwszych 2 GiB (od 1 MiB).
$cflags = $common + @('-mno-red-zone', '-mcmodel=small')
# Silnik JavaScript (src\js_*.c) liczy na liczbach zmiennoprzecinkowych (double),
# wiec dostaje SSE. Reszta kernela (i obsluga przerwan) SSE nie dotyka, a stan
# rejestrow SSE zapisuje przelaczanie zadan (thread.c) - to wystarcza.
$jsflags = @($cflags | Where-Object { $_ -ne '-mno-sse' })

New-Item -ItemType Directory -Force build | Out-Null

$objs = @()
foreach ($src in Get-ChildItem src -Include *.c, *.S -Recurse) {
    $obj = "build\$($src.BaseName)$($src.Extension -replace '\.', '_').o"
    Write-Host "  CC  $($src.Name)"
    $flags = if ($src.Name -like 'js_*.c') { $jsflags } else { $cflags }
    & $clang @flags $src.FullName -o $obj
    if ($LASTEXITCODE) { throw "Blad kompilacji: $($src.Name)" }
    $objs += $obj
}

Write-Host "  LD  build\kernel64.elf"
& $lld -m elf_x86_64 -T linker.ld -o build\kernel64.elf @objs
if ($LASTEXITCODE) { throw 'Blad linkowania' }
# Loader Multiboot w QEMU przyjmuje tylko pliki ELF32 - przepakowujemy ten
# sam kod do "koperty" 32-bitowej. Start i tak jest w trybie 32-bitowym
# (boot.S), a do 64 bitow kernel przechodzi sam.
Write-Host "  OBJCOPY  build\kernel.elf"
& $objcopy -I elf64-x86-64 -O elf32-i386 build\kernel64.elf build\kernel.elf
if ($LASTEXITCODE) { throw 'Blad objcopy' }

Write-Host 'Gotowe: build\kernel.elf' -ForegroundColor Green

# Programy dla NaszOS: kazdy programs\*.c -> build\programs\nazwa.exe
# (ELF64 pod adresem 0x8000000000, razem z biblioteka programs\lib\naszos.c).
# -mcmodel=large: program lezy powyzej 4 GiB, wiec adresy maja pelne 64 bity.
# Przy starcie QEMU przekazuje je jako moduly, a NaszOS kopiuje je do /programy.
New-Item -ItemType Directory -Force build\programs | Out-Null
$pflags = $common + @('-mcmodel=large', '-I', 'programs\lib')
& $clang @pflags programs\lib\naszos.c -o build\programs\naszos.o
if ($LASTEXITCODE) { throw 'Blad kompilacji: programs\lib\naszos.c' }
$programs = @()
foreach ($src in Get-ChildItem programs -Filter *.c) {
    $obj = "build\programs\$($src.BaseName).o"
    $exe = "build\programs\$($src.BaseName).exe"
    Write-Host "  CC  programs\$($src.Name)"
    & $clang @pflags $src.FullName -o $obj
    if ($LASTEXITCODE) { throw "Blad kompilacji: programs\$($src.Name)" }
    & $lld -m elf_x86_64 -T programs\lib\program.ld -o $exe $obj build\programs\naszos.o
    if ($LASTEXITCODE) { throw "Blad linkowania: $exe" }
    $programs += $exe -replace '\\', '/'
}
Write-Host "Gotowe programy: $($programs.Count)" -ForegroundColor Green

# Wirtualny dysk dla NaszOS - tu zapisuja sie pliki. Tworzony tylko raz;
# zeby zaczac od czystego dysku, wystarczy usunac disk.img.
$disk = Join-Path $PSScriptRoot 'disk.img'
$diskSize = 64MB
if (-not (Test-Path $disk)) {
    $f = [IO.File]::Create($disk)
    $f.SetLength($diskSize)
    $f.Close()
    Write-Host 'Utworzono pusty dysk: disk.img (64 MiB)' -ForegroundColor Green
} elseif ((Get-Item $disk).Length -lt $diskSize) {
    # Powiekszenie starego dysku (4 MiB -> 64 MiB), zeby zmiescily sie np. pobrane
    # piosenki. NaszOS trzyma pliki w dwoch "slotach": od poczatku dysku i od jego
    # polowy - drugi slot trzeba wiec przeniesc na nowa polowe.
    $f = [IO.File]::Open($disk, 'Open', 'ReadWrite')
    $oldHalf = [int]($f.Length / 2)
    $slot1 = New-Object byte[] $oldHalf
    $f.Position = $oldHalf
    [void]$f.Read($slot1, 0, $oldHalf)
    $f.SetLength($diskSize)
    $f.Position = $diskSize / 2
    $f.Write($slot1, 0, $oldHalf)
    $f.Close()
    Write-Host 'Powiekszono dysk: disk.img (64 MiB), pliki zachowane' -ForegroundColor Green
}

if ($Run) {
    $qemu = Find-Tool 'qemu-system-x86_64' 'C:\Program Files\qemu'
    # -rtc base=localtime: zegar RTC pokazuje czas lokalny, jak na PC z Windowsem
    # -audiodev dsound + pcspk-audiodev: glosniczek PC gra przez glosniki Windowsa
    # isa-debug-exit: przez port 0xF4 NaszOS mowi "restart" - QEMU konczy sie wtedy
    # z kodem 33, a ta petla uruchamia go od nowa (patrz nizej)
    # -m 6G: 6 GiB RAM - wiecej niz 4 GiB, ktore widzialby system 32-bitowy
    $qemuArgs = @('-kernel', 'build\kernel.elf', '-m', '6G', '-drive', "file=$disk,format=raw,if=ide,index=0",
                  '-rtc', 'base=localtime', '-name', 'NaszOS',
                  '-audiodev', 'dsound,id=glosnik', '-machine', 'pc,pcspk-audiodev=glosnik',
                  '-device', 'AC97,audiodev=glosnik',   # karta dzwiekowa (muzyka, pianino, WAV)
                  '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                  # Karta sieciowa RTL8139 podlaczona do "wirtualnego routera" QEMU:
                  # NaszOS dostaje adres 10.0.2.15, a ruch idzie do internetu przez Windowsa
                  '-netdev', 'user,id=siec0', '-device', 'rtl8139,netdev=siec0')
    if ($programs) {
        $qemuArgs += @('-initrd', ($programs -join ','))   # programy .exe jako moduly Multiboot
    }
    $fullscreen = $false
    if ($Okno) {
        # W oknie tez tryb graficzny (pulpit z oknami), ale w stalej rozdzielczosci
        $qemuArgs += @('-append', 'res=1280x800')
    } else {
        # Na pelnym ekranie NaszOS dostaje rozdzielczosc monitora (tryb graficzny) -
        # litery maja ten sam rozmiar co w oknie, a miesci sie ich wiecej
        $gpu = Get-CimInstance Win32_VideoController |
               Where-Object CurrentHorizontalResolution | Select-Object -First 1
        if ($gpu) {
            $res = "$($gpu.CurrentHorizontalResolution)x$($gpu.CurrentVerticalResolution)"
            $qemuArgs += @('-append', "res=$res")
            Write-Host "Rozdzielczosc monitora: $res" -ForegroundColor Cyan
        }
        # zoom-to-fit=off: obraz 1:1, bez powiekszania. Z wlaczonym dopasowaniem QEMU
        # zostawial powiekszenie 3x w pionie z trybu tekstowego (720x400) po
        # przejsciu na rozdzielczosc monitora i litery byly rozciagniete.
        # show-menubar=off: okno bez menu - fullscreen.ps1 zdejmie mu tez ramke
        # i rozciagnie na caly monitor (pelny ekran bez obramowania).
        $qemuArgs += @('-display', 'gtk,zoom-to-fit=off,show-menubar=off')
        $fullscreen = $true
        Write-Host 'Pelny ekran:  Ctrl+Alt+F = okno/pelny ekran,  Ctrl+Alt+G = uwolnij mysz' -ForegroundColor Cyan
    }

    $quoted = $qemuArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    do {
        $proc = Start-Process $qemu -ArgumentList $quoted -PassThru
        $null = $proc.Handle              # bez tego PowerShell czasem gubi kod wyjscia
        if ($fullscreen) {
            # Gdy NaszOS przelaczy sie na tryb graficzny, QEMU zmienia rozmiar okna.
            # fullscreen.ps1 pilnuje okna przez pierwsze sekundy i za kazdym razem
            # rozciaga je z powrotem na caly monitor.
            & (Join-Path $PSScriptRoot 'fullscreen.ps1') -ProcessId $proc.Id -Watch 25
        }
        $proc.WaitForExit()
        $restart = $proc.ExitCode -eq 33
        if ($restart) { Write-Host 'Restart NaszOS...' -ForegroundColor Cyan }
    } while ($restart)
}
