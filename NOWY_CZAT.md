Cześć! Kontynuujemy projekt **NaszOS** – własny system operacyjny pisany od zera w C, uruchamiany w QEMU na Windows 11. Projekt jest w `C:\Users\relax\windows_exe` (to nie jest repozytorium git).

## Jak ze mną rozmawiać
- Piszę po polsku. Robię to razem z dzieckiem (ok. 20 lat), więc **każde wyjaśnienie ma dwie części**: najpierw „po ludzku (dla młodego)” – prosto, obrazowo – a potem „dla rodzica (technicznie)”.
- Komentarze w kodzie po polsku, **bez polskich znaków** (ASCII), w stylu istniejącego kodu.
- Zanim odpalisz QEMU dla mnie: sprawdź, czy ekran Windowsa nie jest zablokowany (proces na pierwszym planie nie może być `LockApp`). Nie rób zrzutów mojego prawdziwego pulpitu.

## Co już jest (NaszOS 3.0, x86-64)
- Jądro 64-bitowe (Multiboot → long mode), stronicowanie 4-poziomowe, programy użytkownika w ring 3 (ELF64 pod `0x8000000000`, `int 0x80`), wątki kooperacyjne (wątki jądra NIE są wywłaszczane – długie obliczenia muszą wołać `thread_yield()`).
- Pulpit z oknami, Terminal, Pliki, Notatnik, Kalendarz, Snake, **Pianino**, Ustawienia.
- Dźwięk: karta AC'97 + mikser (`ac97.c`, `audio.c`), komendy `graj plik.wav`, `melodia`.
- Sieć: RTL8139, własny TCP/IP, DHCP, DNS, NTP (zegar z internetu), komendy `ping`, `dns`, `www`, `pobierz`, `czas`.
- **HTTPS**: własny TLS 1.3 (`crypto.c`: SHA-256/HMAC/HKDF/X25519/ChaCha20-Poly1305/AES-GCM; `tls.c`). Certyfikaty NIE są sprawdzane. Plan B: FrogFind.
- **Przeglądarka** (`browser.c`) z własnym silnikiem:
  - `dom.c` – parser HTML → drzewo DOM (pamięć z areny `arena.c`, zwalniana ze stroną),
  - `css.c` – arkusze (`<style>`, `<link>`, `style=""`), selektory, kaskada, `var()`, `@media`, ~70 właściwości, flexbox,
  - `layout.c` – model pudełkowy, linie z linią bazową, floaty, tabele, listy, flex → lista rysowania (`struct dl_item`),
  - `image.c` – GIF, PNG (własny DEFLATE), JPEG (baseline + progresywny),
  - czcionki DejaVu wygładzane (`font.c`, `font_data.c` generowane przez `tools/make_font.py`), UTF-8 (`utf8.c`).
  - Strona główna Wikipedii wygląda już bardzo podobnie jak w Chrome.
- **JavaScript** (zrobione w poprzednim czacie – szczegóły niżej).

## Budowanie, uruchamianie, testy
- Narzędzia: LLVM w `C:\Program Files\LLVM\bin` (clang, ld.lld, llvm-objcopy – nie ma ich w PATH narzędzia PowerShell), QEMU w `C:\Program Files\qemu`.
- `.\build.ps1` – buduje; `.\build.ps1 -Run` – uruchamia na pełnym ekranie (6 GB RAM, sieć, AC'97, dysk `disk.img` 64 MB).
- Build traktuje każde ostrzeżenie kompilatora jako błąd (ErrorActionPreference Stop) – kod musi się kompilować bez ostrzeżeń.
- Skrypty testowe w `tools\test` (bez okna, sterowane przez monitor QEMU):
  - `vgadump.ps1 -Disk tools\test\test.img -Keys @(...) -Extra "..."` – tryb tekstowy, wypisuje ekran,
  - `shot.ps1 -Disk ... -Res 1920x1200 -Png plik.png -Keys @('at:63,145','dbl', ...) -Extra "-m 2G -netdev user,id=n0 -device rtl8139,netdev=n0"` – zrzut pulpitu (ikona Przeglądarki: podwójny klik w `at:63,145`; tokeny: `at:X,Y`, `click`, `dbl`, `wheel:-1`, `sleepN`, nazwy klawiszy QEMU),
  - `keys.ps1 -Text "adres`n"` – zamienia tekst na klawisze,
  - `tools\test\www` – strony testowe (CSS, obrazki); serwer: `python -m http.server 8000 --bind 127.0.0.1` w tym folderze, w NaszOS adres `10.0.2.2:8000/...`.
  - Diagnostyka: w QEMU `-debugcon file:debug.txt` + `outb(0xE9, znak)` w kodzie.

## Pułapki, na które już wpadliśmy
- Narzędzie Bash „zjada” podwójne ukośniki (`\\` → `\`) w komendach – przy edycji plików z `'\0'`, `'\n'`, `'\t'` do C przez Pythona powstają prawdziwe znaki NUL/tab. Edytuj narzędziem Edit/Write albo skryptem .py zapisanym przez Write; po edycjach sprawdzaj, czy w `src` nie ma bajtów `\x00` ani `\r`.
- Python na Windows zapisuje CRLF – używaj `open(..., newline='\n')`.
- Stos jądra i wątków: 256 KB; układ strony jest rekurencyjny. JS ma własny stos 8 MB.
- Nazwa klawisza Backspace w QEMU to `backspace` (nie `backspc`). W `shot.ps1` `wheel:-1` przewija stronę w dół.
- W testach klikania pamiętaj, że strona przesuwa się po zmianach DOM (np. dodaniu pozycji listy) – najpierw zrzut, potem współrzędne.
- Dwie karty PCI mogą dzielić IRQ – `interrupt_register` wspiera kilka funkcji na jedno przerwanie, każda sprawdza swoje urządzenie.

## ZROBIONE: JavaScript w przeglądarce (poprzedni czat)
Przeglądarka wykonuje skrypty stron (`<script>` wbudowane i z `src=`). Silnik ok. 16 tys. linii C.

**Jądro – liczby zmiennoprzecinkowe**: `boot.S` włącza SSE (CR0.EM=0, MP=1, CR4.OSFXSR|OSXMMEXCPT, `fninit`); `thread.c` ma 512 B `fpu[]` na wątek, `fxsave`/`fxrstor` wokół `context_switch`, nowy wątek `fninit`+`ldmxcsr`. W `build.ps1` tylko pliki `js_*.c` są kompilowane bez `-mno-sse`. Interfejs `js.h` nie przekazuje `double`.

**Pliki silnika** (`src/`):
- `js.h` – API dla przeglądarki/terminala; `js_int.h` – wnętrze (wartość `jv` 16 B, `jstr` UTF-16, `jobj`, środowiska, AST).
- `js_lex.c` + `js_lex.h` – tokeny, ASI, szablony, regexy, komentarze `<!--`.
- `js_parse.c` – AST (zejście rekurencyjne): let/const, strzałki, klasy (pola, `#prywatne`, static, extends/super), destrukturyzacja, spread/rest, `?.`, `??`, `**`, szablony i tagged templates, etykiety. Brak: async/await, generatory, moduły (dają SyntaxError).
- `js_eval.c` – interpreter drzewiasty; wyjątki przez `js_setjmp/js_longjmp` (`js_asm.S`), własny stos 8 MB (`js_on_stack`), kontrola głębokości (rsp), limit czasu (`js_step` → `js_check_time`: `thread_yield` co 30 ms, przerwanie po 8 s skrypt / 3 s zdarzenie), `js_protect`.
- `js_core.c` – pamięć (arena, limit 256 MB, BEZ GC), teksty (doklejanie bez kopiowania), atomy, właściwości z tablicą mieszającą, konwersje, błędy.
- `js_num.c` – najkrótszy zapis liczb (Burger–Dybvig na dużych liczbach), dokładny strtod, toFixed/toPrecision, sin/cos/tan/exp/log/atan z fdlibm.
- `js_lib.c` / `js_lib2.c` – Object, Function, Array, String, Number, Boolean, Math, JSON, Date (czas polski, `toLocale*String('pl-PL')`), Error*, Map/Set (z hashowaniem), Promise + mikrozadania, Symbol (uproszczony: teksty `@@...`), console, performance.
- `js_regex.c` – wyrażenia regularne (nawroty na własnym stosie, grupy nazwane, lookahead/lookbehind, flagi gimsuy) + replace/match/matchAll/search/split.
- `js_dom.c` – DOM (document, Node/Element/Text, classList, style, dataset, innerHTML/outerHTML, querySelector przez `css_sel_parse`), zdarzenia (capture/target/bubbling, `onclick` z atrybutów, once, preventDefault), timery i requestAnimationFrame, window/location/navigator/history/localStorage (w pamięci)/cookie, alert/confirm/prompt, pisanie w polach `<input>`, publiczne API (`js_new`, `js_run_script`, `js_click`, `js_key`, `js_timers`...).
- `shell_js.c` – komenda terminala `js` (`js 2+2`, `js plik.js`, samo `js` = tryb interaktywny).

**Zmiany w istniejących plikach**:
- `dom.c/h` – węzły `<script>` w drzewie i lista `d->scripts`; `dom_parse_scripts()` wykonuje skrypt w chwili, gdy parser na niego trafi; `dom_parse_fragment()` (innerHTML); funkcje zmiany drzewa; `<noscript>` pomijany przy włączonym JS; pole `js` i `script_done` w węźle.
- `css.c/h` – API selektorów (`css_sel_parse/match`), selektory `+ ~ :not() :nth-child() :nth-last-child :only-child :first/last-of-type :empty`; style używane ponownie przy przeliczaniu; `position` (relative/absolute/fixed) + `top/right/bottom/left/inset`, `transform: translate`, `opacity: 0` = ukryte, `white-space: pre-line`, `line-through`; domyślne marginesy 0 (wcześniej błędnie `auto` – elementy z szerokością były centrowane).
- `layout.c/h` – `dl_item.node` (który element narysowano – do kliknięć), układanie elementów absolute/fixed (względem przodka z `position`), przesunięcia relative i translate.
- `browser.c` – skrypty w wątku ładowania (z `src=` przez `http_get`), potem JS pod opieką okna: kliknięcia, klawisze (strony z grami dostają litery i strzałki), timery w `browser_tick`, po zmianach DOM `css_compute` + nowy układ, okienko `alert`, `console.log` na pasku stanu i porcie 0xE9, obrazki dodane przez skrypt, wbudowana strona **`naszos:js`** (licznik, kolory, zegar, kalkulator, wąż).
- `shell.c/h` – komenda `js`, `shell_read_line()`.

**Testy**:
- `tools/jstest` – ten sam kod jako program dla Windows (atrapy jądra w `shim.c`/`stubs.c`, import kernel32 przez `llvm-dlltool`). Budowanie: `sh tools/jstest/build.sh` (Git Bash). Uruchamianie: `tools/jstest/jstest.exe plik.js`, `jstest.exe strona.html` (DOM + CSS + układ + timery 2 s), `jstest.exe -e "kod"`.
- Zestawy w `tools/jstest/tests`: `t1_basics.js` (147), `t2_functions.js` (78), `t3_library.js` (176), `t4_async.js` (kolejność Promise/timerów), `t5_dom.html` (70) – wszystkie przechodzą.
- Strony w `tools\test\www`: `js_licznik.html` (+ `js_licznik.js`, `js_styl.css`), `pozycje.html` – sprawdzone w QEMU przez serwer `10.0.2.2:8000`.
- Prawdziwe strony sprawdzone w QEMU: Google (wyniki, 15 skryptów), Wikipedia (strona główna), example.com, 68k.news, info.cern.ch.

**Czego jeszcze nie ma / znane problemy**:
- Brak odśmiecania pamięci (GC) – długo działająca gra po kilku godzinach dojdzie do limitu 256 MB i jej skrypt zostanie przerwany.
- `fetch` i XMLHttpRequest zawsze zwracają błąd; brak `<canvas>`, async/await, generatorów.
- `Math.pow` z ułamkowym wykładnikiem może różnić się od Chrome na ostatniej cyfrze.
- Litery spoza czcionki (chińskie, arabskie, część ikon) jako „?”.
- Nagłówek artykułów Wikipedii (link „Przejdź do zawartości”, pusta przestrzeń, spis treści na całą szerokość) – nie wynika z JS, pozycjonowania ani marginesów; niesprawdzone, czy to nowe selektory CSS, czy było tak zawsze.
- Mini-gry w `tools\test\www` nie robiliśmy (gra jest na `naszos:js`).
