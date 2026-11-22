# Zamienia tekst na nazwy klawiszy dla "sendkey" w QEMU
param([string]$Text)
$map = @{ ' '='spc'; '.'='dot'; '/'='slash'; ':'='shift-semicolon'; '-'='minus'; '_'='shift-minus'; '%'='shift-5'; '?'='shift-slash'; '='='equal'; "`n"='ret'
          '*'='shift-8'; '('='shift-9'; ')'='shift-0'; '+'='shift-equal'; ','='comma'; ';'='semicolon'
          '['='bracket_left'; ']'='bracket_right'; '{'='shift-bracket_left'; '}'='shift-bracket_right'
          '"'='shift-apostrophe'; "'"='apostrophe'; '<'='shift-comma'; '>'='shift-dot'; '!'='shift-1'
          '@'='shift-2'; '#'='shift-3'; '$'='shift-4'; '^'='shift-6'; '&'='shift-7'; '|'='shift-backslash'
          '\'='backslash'; '`'='grave_accent'; '~'='shift-grave_accent' }
$Text.ToCharArray() | ForEach-Object { $c = [string]$_; if ($map.ContainsKey($c)) { $map[$c] } elseif ($c -cmatch '[A-Z]') { "shift-$($c.ToLower())" } else { $c } }
