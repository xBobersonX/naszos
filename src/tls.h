#ifndef TLS_H
#define TLS_H

#include <stdint.h>

/* TLS 1.3 - szyfrowane polaczenie dla HTTPS (src/tls.c), na polaczeniu TCP
 * z net.c. Jedno polaczenie naraz (jak TCP).
 *
 * UWAGA: NaszOS NIE sprawdza certyfikatu serwera (nie ma listy zaufanych
 * urzedow certyfikacji ani weryfikacji podpisow RSA/ECDSA). Polaczenie jest
 * szyfrowane, ale ktos "po drodze" moglby sie podszyc pod serwer. Do nauki
 * i czytania stron - tak; do logowania i hasel - nie. */

#define TLS_EPROTO    -30     /* serwer przyslal cos niezrozumialego */
#define TLS_EVERSION  -31     /* serwer nie zna TLS 1.3 */
#define TLS_EGROUP    -32     /* serwer chce innej krzywej niz X25519 */
#define TLS_EALERT    -33     /* serwer przerwal (alert) */
#define TLS_EDECRYPT  -34     /* zly znacznik - dane zmienione albo blad */
#define TLS_ECIPHER   -35     /* brak wspolnego szyfru */

/* Po tcp_connect(): uzgadnia klucze. host - nazwa serwera (SNI). 0 = OK */
int  tls_connect(const char *host);
int  tls_send(const void *data, unsigned len);
/* >0 = ile bajtow, 0 = koniec polaczenia, <0 = blad */
int  tls_recv(void *buf, unsigned size, uint32_t timeout_ms);
void tls_close(void);

const char *tls_strerror(int err);
const char *tls_cipher_name(void);   /* np. "TLS 1.3, ChaCha20-Poly1305" */
int tls_alert_code(void);            /* numer alertu serwera (przy TLS_EALERT) */

#endif
