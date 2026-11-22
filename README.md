# NaszOS

Własny system operacyjny napisany od zera w C (i odrobinie asemblera), na komputery x86-64.
Ma pulpit z oknami, terminal, sieć z HTTPS, dźwięk i przeglądarkę stron WWW
z własnym silnikiem HTML/CSS/JavaScript. Nie korzysta z żadnej gotowej biblioteki:
nie ma libc, nie ma Linuksa pod spodem, wszystko jest napisane samodzielnie.

Uruchamia się w emulatorze [QEMU](https://www.qemu.org/) na Windowsie.

## Co potrafi

**Jądro**
- 64 bity (start przez Multiboot, potem przejście do long mode), stronicowanie 4-poziomowe
- programy użytkownika w ring 3 (ELF64, wywołania systemowe przez `int 0x80`)
- wątki kooperacyjne, przerwania, zegar RTC, zapis plików na wirtualnym dysku

**Pulpit i aplikacje**
- okna, mysz, klawiatura (z polskimi znakami), czcionki DejaVu z wygładzaniem
- Terminal, Pliki, Notatnik, Kalendarz, Ustawienia, Pianino, Snake
- programy w osobnych plikach (`programs/`): kalkulator, zgadnij, piłka, liczby, witaj, awaria

**Dźwięk**
- karta AC'97 z mikserem i głośniczek PC; w terminalu `graj plik.wav` i `melodia`

**Sieć**
- karta RTL8139 i własny stos TCP/IP: DHCP, DNS, NTP (zegar z internetu)
- własny TLS 1.3 (SHA-256, HKDF, X25519, ChaCha20-Poly1305, AES-GCM), dzięki czemu działa HTTPS
- komendy `ping`, `dns`, `www`, `pobierz`, `czas`

**Przeglądarka**
- parser HTML do drzewa DOM
- CSS: selektory, kaskada, `var()`, `@media`, flexbox, `position`, około 70 właściwości
- układ strony: model pudełkowy, floaty, tabele, listy
- obrazki GIF, PNG (z własnym DEFLATE) i JPEG (baseline i progresywny)
- **JavaScript**: własny interpreter (ok. 16 tys. linii) z klasami, destrukturyzacją,
  Promise, wyrażeniami regularnymi, JSON, Date, Map/Set i API DOM ze zdarzeniami i timerami
- **`<canvas>`** (kontekst `2d`): ścieżki, łuki, krzywe Béziera, wygładzane krawędzie, linie
  z połączeniami i zakończeniami, gradienty liniowe i radialne, przezroczystość i tryby mieszania,
  przekształcenia (`translate`/`rotate`/`scale`), `clip`, tekst, `drawImage`
  (z `<img>` i innego płótna), `getImageData`/`putImageData`; obrazki mają `onload` i `complete`
- **`fetch` i `XMLHttpRequest`**: pobieranie danych skryptem w tle (HTTP i HTTPS), `GET`/`POST`/inne
  metody, własne nagłówki, `Response` z `.json()`, `.text()`, `.headers`, `.ok`, `.status`
- działają m.in. Google, strona główna Wikipedii, example.com; wbudowana strona demo `naszos:js`

## Jak zbudować i uruchomić

Potrzebne programy (Windows):
- [LLVM](https://releases.llvm.org/) (clang, ld.lld, llvm-objcopy), domyślnie w `C:\Program Files\LLVM\bin`
- [QEMU](https://www.qemu.org/download/#windows), domyślnie w `C:\Program Files\qemu`

```powershell
.\build.ps1              # tylko kompilacja -> build\kernel.elf
.\build.ps1 -Run         # kompilacja + uruchomienie na pełnym ekranie
.\build.ps1 -Run -Okno   # to samo, ale w oknie 1280x800
```

Przy pierwszym uruchomieniu powstaje pusty dysk `disk.img` (64 MB), na którym NaszOS
zapisuje pliki. Żeby zacząć od czystego dysku, wystarczy go usunąć.

Skróty QEMU: `Ctrl+Alt+F` przełącza okno i pełny ekran, `Ctrl+Alt+G` uwalnia mysz.

## Struktura projektu

| Folder / plik | Co zawiera |
|---|---|
| `src/` | jądro, sterowniki, pulpit, sieć, przeglądarka, silnik JS |
| `programs/` | programy użytkownika i ich biblioteka (`programs/lib`) |
| `fonts/` | licencja czcionek DejaVu |
| `tools/make_font.py` | generuje `src/font_data.c` z czcionek |
| `tools/jstest/` | silnik JS jako program dla Windowsa + testy |
| `tools/test/` | skrypty testów w QEMU i strony testowe (`www/`) |
| `build.ps1`, `linker.ld` | budowanie jądra i programów |

## Testy

- Silnik JavaScript: `sh tools/jstest/build.sh` (Git Bash), potem
  `tools/jstest/jstest.exe tools/jstest/tests/t1_basics.js` (zestawy `t1`–`t7`: `t6_canvas.html` to canvas,
  `t7_fetch.html` to fetch i XMLHttpRequest).
  `jstest.exe -canvas obraz.bmp strona.html` zapisuje pierwsze płótno strony do pliku BMP.
- Strony testowe: w folderze `tools/test/www` uruchom
  `python -m http.server 8000 --bind 127.0.0.1`, a w przeglądarce NaszOS wpisz `10.0.2.2:8000/...`
  (np. `10.0.2.2:8000/canvas.html` – rysunek, animacja i klikanie na płótnie,
  `10.0.2.2:8000/fetch.html` – zapytania fetch, także do internetu).

## Licencja

[MIT](LICENSE) – możesz używać, zmieniać i rozpowszechniać kod, podając autora.
Czcionki DejaVu mają własną licencję ([fonts/LICENSE_DEJAVU.txt](fonts/LICENSE_DEJAVU.txt)).

## Czego jeszcze nie ma

- odśmiecania pamięci w JS (bardzo długo działający skrypt w końcu zostanie przerwany)
- `async`/`await`, generatorów
- w fetch: plików binarnych (obrazki przez fetch), `AbortController`, sprawdzania CORS
- w canvas: cieni, linii przerywanych, wzorów (`createPattern`), `Path2D`, WebGL
- sprawdzania certyfikatów HTTPS
- znaków spoza czcionki (np. chińskich), które wyświetlają się jako „?”
