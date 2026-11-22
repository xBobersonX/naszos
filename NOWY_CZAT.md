Cześć! Kontynuujemy projekt **NaszOS** – własny system operacyjny pisany od zera w C, uruchamiany w QEMU na Windows 11. Projekt to repozytorium git, wysyłane na GitHub: https://github.com/xBobersonX/naszos (gałąź `main`; commity i push tylko na moją prośbę).

## Jak ze mną rozmawiać
- Piszę po polsku. **Każde wyjaśnienie ma dwie części**: najpierw „po ludzku” – prosto, obrazowo – a potem część techniczną.
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
- `js_parse.c` – AST (zejście rekurencyjne): let/const, strzałki, klasy (pola, `#prywatne`, static, extends/super), destrukturyzacja, spread/rest, `?.`, `??`, `**`, szablony i tagged templates, etykiety. Brak: generatory, moduły (dają SyntaxError). async/await – patrz niżej.
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
- Zestawy w `tools/jstest/tests`: `t1_basics.js` (147), `t2_functions.js` (78), `t3_library.js` (176), `t4_async.js` (kolejność Promise/timerów), `t5_dom.html` (70), `t6_canvas.html` (89), `t7_fetch.html` (40), `t8_await.js` (35) – wszystkie przechodzą.
- Strony w `tools\test\www`: `js_licznik.html` (+ `js_licznik.js`, `js_styl.css`), `pozycje.html` – sprawdzone w QEMU przez serwer `10.0.2.2:8000`.
- Prawdziwe strony sprawdzone w QEMU: Google (wyniki, 15 skryptów), Wikipedia (strona główna), example.com, 68k.news, info.cern.ch.

## ZROBIONE: `<canvas>` (kontekst 2d)
- `src/js_canvas.c` – cały kontekst 2d (kompilowany z SSE jak inne `js_*.c`). Piksele płótna: `struct dom_canvas` w `dom.h` (0xAARRGGBB bez mnożenia przez alfę, `kmalloc`, zwalniane w `dom_free`), `node->canvas`, tworzone przez `dom_canvas_sync()` z atrybutów width/height (domyślnie 300x150).
- Rysowanie: ścieżka w pikselach płótna (punkty przeliczane macierzą przy dodawaniu), wypełnianie scanline z 4 podwierszami i dokładnym pokryciem w poziomie (wygładzanie), nonzero/evenodd. Stroke = wielokąty (prostokąt na odcinek + miter/round/bevel + butt/round/square), wszystkie skierowane w tę samą stronę i wypełniane nonzero. Gradienty przez tablicę 256 kolorów. `clip()` = maski bajtowe, jedna na poziom `save()`. Tekst = bitmapy DejaVu (`font_glyph()` w `font.h`) próbkowane dwuliniowo przez odwrotną macierz (działa obrót i skala).
- Bufory robocze w kontekście są używane ponownie (`grow()`), żeby animacje nie zjadały pamięci JS (brak GC!). `getImageData` tworzy zwykłą tablicę JS (16 B na liczbę) – duże i częste wywołania szybko zjedzą limit 256 MB.
- Układ: `DL_CANVAS` w `layout.h`; `<canvas>` i `<img>` jako blok/float/flex-item idą przez `layout_box` → `replaced_atom` (wcześniej obrazki z `display:block` i `float` były niewidoczne!). Przeglądarka: `gfx_blit_alpha()` w `gfx.c`; `doc->canvas_gen` rośnie po każdym rysowaniu → `js_changes()` odświeża okno.
- Obrazki dla `drawImage`: `host.image` (`b_image` w `browser.c`); `dom_doc.image_nodes[]` + `js_image_done()` dają `img.onload`/`onerror` (z `poll_images`). `img.complete`, `naturalWidth/Height`. `offsetX/offsetY` w zdarzeniach myszy są teraz względem klikniętego elementu.
- Testy: `tools/jstest/tests/t6_canvas.html` (89 sprawdzeń pikseli przez getImageData), `jstest.exe -canvas plik.bmp strona.html` zapisuje płótno do BMP. Strona `tools/test/www/canvas.html` (rysunek, animowana piłka, kółka w miejscu kliknięć, zdjęcie przez drawImage) – sprawdzona w QEMU.
- Brakuje: cieni, `setLineDash` (ignorowane), `createPattern` (null), `Path2D`, `toDataURL` (zwraca "data:,"), `isPointInStroke` (false), WebGL. Tryby mieszania typu multiply/screen rysują jak source-over.

## ZROBIONE: `fetch` i `XMLHttpRequest`
- `src/js_net.c`: `fetch` tylko dopisuje zapytanie do `rt->net[]` (max 64) i zwraca Promise. Przeglądarka (`poll_fetch` w `browser.c`, wołane z `browser_tick`) bierze je przez `js_take_request`, pobiera w wątku `fetch_thread` przez `http_request` i oddaje przez `js_request_done` → `Response` (`text/json/arrayBuffer/clone`, `headers.get/has/forEach/entries`, `ok/status/statusText/url`) albo zdarzenia XHR (`readystatechange` 1-4, `load/error/abort/loadend`, `responseType: "json"`, `getResponseHeader`).
- Jedno zapytanie naraz na okno (sieć i tak ma `net_lock`). Zmiana strony / zamknięcie okna: `cancel_fetch` – wątek sprząta sam, odpowiedź nie trafia do nowego świata JS.
- `src/http.c`: `http_request(url, &http_req{method, headers, body}, ...)` – POST/PUT itd., `Content-Length`, przekierowania 301/302/303 → GET; `http_get` to teraz jego nakładka. `http_header()` czyta nagłówek odpowiedzi.
- Nagłówki z JS są czyszczone z `\r\n` (żeby skrypt nie wstrzyknął własnych). Brak sprawdzania CORS i ciasteczek w fetch. Treść odpowiedzi traktowana jako UTF-8 (pliki binarne się psują).
- jstest: `serve_requests()` w `tools/jstest/main.c` – adresy `.../echo` (odsyła metodę|nagłówki|treść), `.../fail` (błąd sieci), reszta = pliki obok strony. Test `t7_fetch.html` (36 sprawdzeń). Strona `tools/test/www/fetch.html` sprawdzona w QEMU (lokalny JSON, XHR, 404, https://example.com).
- Uwaga przy testach w QEMU: pierwszy podwójny klik po starcie czasem ginie – najpierw otwórz Terminal (`at:63,45`, `dbl`), potem Przeglądarkę.

## ZROBIONE: `async` / `await`
- `src/js_async.c`: każde wywołanie funkcji async to korutyna z **własnym stosem C** (1 MB, kmalloc, pula 8 zwolnionych stosów, max 400 czekających naraz). Start przez `js_on_stack`, przełączanie przez `js_setjmp/js_longjmp`. `await` = `.then(wznów, wznów-z-błędem)` na obietnicy + uśpienie; mikrozadanie budzi korutynę (`co_react` → `coro_resume`). Koniec ciała → resolve/reject obietnicy wyniku.
- Na czas działania korutyny podmieniane są: `rt->catch_top`, `stack_limit` (zapas 128 KB → RangeError ok. 650 poziomów rekurencji wewnątrz async), `depth`, `line`, `ret`, `label`, `cur_coro`. Przerwanie (czas/pamięć) w korutynie idzie dalej do wznawiającego (`CO_ABORT`). `js_free` zwalnia stosy także uśpionych korutyn.
- Parser: `async function` (deklaracje i wyrażenia), `async x =>`, `async (...) =>` (zaglądanie za nawias), metody async w obiektach i klasach (też `static`), `for await (x of tablica)`, `await` jako operator tylko w funkcji async (poza nią zwykła nazwa; `await coś` poza async = SyntaxError). `new` na funkcji async = TypeError.
- Odrzucone obietnice bez `.catch` → w konsoli „Uncaught (in promise) …” (`js_report_unhandled`, po mikrozadaniach w `protect_inner`).
- Testy: `t8_await.js` (35 sprawdzeń), w `t7_fetch.html` dodane `await fetch` (razem 40). Demo `tools/test/www/async.html` (sprawdzone w jstest; w QEMU jeszcze nie – ekran był zablokowany).
- Brak: generatorów (`function*`, `yield`), generatorów async, `Symbol.asyncIterator` (for await iteruje zwykłą iteracją i czeka na każdy element).

**Czego jeszcze nie ma / znane problemy** (JavaScript):
- Brak odśmiecania pamięci (GC) – długo działająca gra po kilku godzinach dojdzie do limitu 256 MB i jej skrypt zostanie przerwany.
- Brak generatorów (parser odrzuca je z SyntaxError).
- `Math.pow` z ułamkowym wykładnikiem może różnić się od Chrome na ostatniej cyfrze.
- Litery spoza czcionki (chińskie, arabskie, część ikon) jako „?”.
- Nagłówek artykułów Wikipedii (link „Przejdź do zawartości”, pusta przestrzeń, spis treści na całą szerokość) – nie wynika z JS, pozycjonowania ani marginesów; niesprawdzone, czy to nowe selektory CSS, czy było tak zawsze.
- Mini-gry w `tools\test\www` nie robiliśmy (gra jest na `naszos:js`).
