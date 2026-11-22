# Ustawia okno QEMU z NaszOS na caly monitor - "pelny ekran bez obramowania":
# zdejmujemy oknu ramke i pasek tytulu i rozciagamy je na caly ekran. Nie trzeba
# do tego wysylac skrotow klawiszowych ani aktywowac okna, wiec dziala zawsze,
# nawet gdy piszesz akurat w innym programie.
#
#   .\fullscreen.ps1               - ustaw pelny ekran teraz
#   .\fullscreen.ps1 -Watch 25     - pilnuj przez 25 s od startu QEMU (uzywa build.ps1):
#                                    NaszOS przy starcie zmienia rozdzielczosc, a QEMU
#                                    zmienia wtedy rozmiar okna - poprawiamy to od razu
param([int]$ProcessId = 0, [int]$Watch = 0)

Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public class QemuWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int i, int v);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);

    const int GWL_STYLE = -16;
    const int FRAME = 0x00C00000 | 0x00040000 | 0x00080000 | 0x00020000 | 0x00010000;
    /* pasek tytulu | ramka do zmiany rozmiaru | menu okna | przyciski min/max */

    public static bool Covers(IntPtr h, int w, int hgt) {
        RECT r;
        if ((GetWindowLong(h, GWL_STYLE) & 0x00C00000) != 0 || !GetWindowRect(h, out r)) return false;
        return r.L == 0 && r.T == 0 && r.R - r.L == w && r.B - r.T == hgt;
    }

    public static void MakeBorderless(IntPtr h, int w, int hgt) {
        SetWindowLong(h, GWL_STYLE, GetWindowLong(h, GWL_STYLE) & ~FRAME);
        SetWindowPos(h, IntPtr.Zero, 0, 0, w, hgt, 0x0020 | 0x0040);   /* FRAMECHANGED | SHOWWINDOW */
    }
}
'@

$proc = if ($ProcessId) { Get-Process -Id $ProcessId -ErrorAction SilentlyContinue }
        else { Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $proc) { Write-Host 'NaszOS (QEMU) nie dziala.'; exit 1 }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

function Get-Handle { $proc.Refresh(); return $proc.MainWindowHandle }

if ($Watch -le 0) {
    $h = Get-Handle
    if ($h -eq [IntPtr]::Zero) { Write-Host 'Okno NaszOS jeszcze sie nie pojawilo.'; exit 1 }
    [QemuWin]::MakeBorderless($h, $screen.Width, $screen.Height)
    Start-Sleep -Milliseconds 300
    if ([QemuWin]::Covers((Get-Handle), $screen.Width, $screen.Height)) { Write-Host 'Pelny ekran: OK'; exit 0 }
    Write-Host 'Nie udalo sie - kliknij w okno NaszOS i nacisnij Ctrl+Alt+F.'
    exit 1
}

# Pilnowanie: konczymy, gdy okno stabilnie zajmuje caly ekran przez 4 s
# (i minelo co najmniej 8 s od startu - NaszOS na pewno zmienil juz rozdzielczosc)
$deadline = $proc.StartTime.AddSeconds($Watch)
$stableSince = $null
while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
    $h = Get-Handle
    if ($h -ne [IntPtr]::Zero) {
        if ([QemuWin]::Covers($h, $screen.Width, $screen.Height)) {
            if (-not $stableSince) { $stableSince = Get-Date }
            $sinceStart = ((Get-Date) - $proc.StartTime).TotalSeconds
            if ($sinceStart -ge 8 -and ((Get-Date) - $stableSince).TotalSeconds -ge 4) { exit 0 }
        } else {
            $stableSince = $null
            [QemuWin]::MakeBorderless($h, $screen.Width, $screen.Height)
        }
    }
    Start-Sleep -Milliseconds 250
}
exit 0
