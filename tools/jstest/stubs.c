/* Test na Windowsie: siec nie istnieje - http.c jest linkowany tylko dla
 * funkcji adresow (url_resolve itp.), wiec tych funkcji nikt nie wola. */
int net_lock(void) { return -1; }
int net_unlock(void) { return -1; }
int net_resolve(void) { return -1; }
int net_format_ip(void) { return -1; }
int tcp_connect(void) { return -1; }
int tcp_send(void) { return -1; }
int tcp_recv(void) { return -1; }
int tcp_close(void) { return -1; }
int tls_connect(void) { return -1; }
int tls_send(void) { return -1; }
int tls_recv(void) { return -1; }
int tls_close(void) { return -1; }
const char *tls_strerror(void) { return "brak sieci"; }
const char *tls_cipher_name(void) { return ""; }

/* rysowanie liter - w tescie nic nie rysujemy */
void gfx_alpha_mask(void) {}
