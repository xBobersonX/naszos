/* TLS 1.3 (RFC 8446) - wersja edukacyjna, patrz tls.h.
 *
 * Przebieg "powitania" (handshake):
 *   my     -> ClientHello: losowe liczby, obslugiwane szyfry i nasza
 *             polowa klucza X25519 (klucz publiczny)
 *   serwer -> ServerHello: wybrany szyfr i jego polowa klucza
 *   Obie strony licza z tego ten sam wspolny sekret (X25519) - kto
 *   podsluchiwal, zna tylko klucze publiczne i sekretu nie policzy.
 *   Z sekretu i "zapisu rozmowy" (transcript - skrot SHA-256 wszystkich
 *   wiadomosci powitania) powstaja klucze szyfrowania (HKDF).
 *   serwer -> (juz zaszyfrowane) EncryptedExtensions, Certificate,
 *             CertificateVerify, Finished - Finished dowodzi, ze serwer zna
 *             klucze i ze nikt nie zmienil rozmowy
 *   my     -> Finished
 * Potem dane aplikacji (HTTP) plyna zaszyfrowane "rekordami" do 16 KB,
 * kazdy z pieczecia (znacznik AEAD), ktora wykrywa zmiany. */

#include "tls.h"
#include "net.h"
#include "crypto.h"
#include "heap.h"
#include "string.h"

#define REC_HANDSHAKE   22
#define REC_APPDATA     23
#define REC_ALERT       21
#define REC_CCS         20

#define HS_CLIENT_HELLO  1
#define HS_SERVER_HELLO  2
#define HS_NEW_TICKET    4
#define HS_ENC_EXT       8
#define HS_CERTIFICATE   11
#define HS_CERT_REQUEST  13
#define HS_CERT_VERIFY   15
#define HS_FINISHED      20
#define HS_KEY_UPDATE    24

#define SUITE_AES128GCM  0x1301
#define SUITE_CHACHA     0x1303

#define REC_MAX   (16384 + 256)
#define HSBUF_MAX 65536

struct keys {
    uint8_t key[32], iv[12], secret[32];
    uint64_t seq;
};

static struct {
    int active;
    int suite, key_len;
    struct keys rd, wr;                  /* odczyt (od serwera) i zapis (do serwera) */
    struct sha256 transcript;
    uint8_t *hsbuf;                      /* skladanie wiadomosci powitania z rekordow */
    unsigned hs_len;
    uint8_t *app;                        /* odszyfrowane dane czekajace na odczyt */
    unsigned app_len, app_pos;
    int peer_closed, alert;
    uint32_t timeout;
} t;

static uint8_t rec[5 + REC_MAX];

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

const char *tls_strerror(int err)
{
    switch (err) {
    case TLS_EPROTO:   return "blad protokolu TLS";
    case TLS_EVERSION: return "serwer nie zna TLS 1.3";
    case TLS_EGROUP:   return "serwer chce innej krzywej niz X25519";
    case TLS_EALERT:   return "serwer przerwal szyfrowanie (alert TLS)";
    case TLS_EDECRYPT: return "nie udalo sie odszyfrowac (zly znacznik)";
    case TLS_ECIPHER:  return "brak wspolnego szyfru";
    default:           return net_strerror(err);
    }
}

const char *tls_cipher_name(void)
{
    return t.suite == SUITE_CHACHA ? "TLS 1.3, X25519, ChaCha20-Poly1305" : "TLS 1.3, X25519, AES-128-GCM";
}

int tls_alert_code(void) { return t.alert; }

/* ---------- harmonogram kluczy (key schedule) ---------- */

static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, unsigned ctx_len,
                         uint8_t *out, unsigned len)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
    unsigned n = 0, ll = (unsigned)strlen(label);
    info[n++] = (uint8_t)(len >> 8);
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)ctx_len;
    memcpy(info + n, ctx, ctx_len);
    n += ctx_len;
    hkdf_expand(secret, info, n, out, len);
}

static void derive_secret(const uint8_t secret[32], const char *label, const uint8_t hash[32], uint8_t out[32])
{
    expand_label(secret, label, hash, 32, out, 32);
}

static void transcript_hash(uint8_t out[32])
{
    struct sha256 copy = t.transcript;   /* skrot "do tej chwili" - oryginal liczy dalej */
    sha256_final(&copy, out);
}

static void set_keys(struct keys *k, const uint8_t secret[32])
{
    memcpy(k->secret, secret, 32);
    expand_label(secret, "key", 0, 0, k->key, (unsigned)t.key_len);
    expand_label(secret, "iv", 0, 0, k->iv, 12);
    k->seq = 0;
}

static void nonce_for(const struct keys *k, uint8_t nonce[12])
{
    memcpy(nonce, k->iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(k->seq >> (8 * i));    /* IV xor numer rekordu */
}

/* ---------- rekordy ---------- */

static int read_exact(uint8_t *p, unsigned n)
{
    while (n) {
        int r = tcp_recv(p, n, t.timeout);
        if (r == 0)
            return NET_ERESET;
        if (r < 0)
            return r;
        p += r;
        n -= (unsigned)r;
    }
    return 0;
}

/* Czyta rekord do rec. Szyfrowany odszyfrowuje; *type = typ tresci. */
static int read_record(uint8_t *type, uint8_t **data, unsigned *len)
{
    int err = read_exact(rec, 5);
    if (err)
        return err;
    unsigned n = be16(rec + 3);
    if (n > REC_MAX)
        return TLS_EPROTO;
    err = read_exact(rec + 5, n);
    if (err)
        return err;
    *type = rec[0];
    *data = rec + 5;
    *len = n;
    if (rec[0] != REC_APPDATA || !t.active)
        return 0;

    if (n < 17)
        return TLS_EPROTO;
    uint8_t nonce[12];
    nonce_for(&t.rd, nonce);
    int bad = t.suite == SUITE_CHACHA
        ? chacha20poly1305_decrypt(t.rd.key, nonce, rec, 5, rec + 5, n - 16, rec + 5, rec + 5 + n - 16)
        : aes128gcm_decrypt(t.rd.key, nonce, rec, 5, rec + 5, n - 16, rec + 5, rec + 5 + n - 16);
    if (bad)
        return TLS_EDECRYPT;
    t.rd.seq++;
    /* Na koncu: prawdziwy typ tresci i ewentualnie zera (dopelnienie) */
    unsigned plen = n - 16;
    while (plen && rec[5 + plen - 1] == 0)
        plen--;
    if (!plen)
        return TLS_EPROTO;
    *type = rec[5 + plen - 1];
    *len = plen - 1;
    return 0;
}

static int write_record(uint8_t type, const uint8_t *data, unsigned len)
{
    static uint8_t out[5 + 16384 + 1 + 16];
    if (!t.active) {                     /* jawny (ClientHello, ChangeCipherSpec) */
        out[0] = type;
        out[1] = 3;
        out[2] = type == REC_HANDSHAKE ? 1 : 3;
        out[3] = (uint8_t)(len >> 8);
        out[4] = (uint8_t)len;
        memcpy(out + 5, data, len);
        return tcp_send(out, 5 + len);
    }
    static uint8_t plain[16384 + 1];
    memcpy(plain, data, len);
    plain[len] = type;                   /* prawdziwy typ ukryty w srodku */
    unsigned clen = len + 1 + 16;
    out[0] = REC_APPDATA;
    out[1] = 3;
    out[2] = 3;
    out[3] = (uint8_t)(clen >> 8);
    out[4] = (uint8_t)clen;
    uint8_t nonce[12];
    nonce_for(&t.wr, nonce);
    if (t.suite == SUITE_CHACHA)
        chacha20poly1305_encrypt(t.wr.key, nonce, out, 5, plain, len + 1, out + 5, out + 5 + len + 1);
    else
        aes128gcm_encrypt(t.wr.key, nonce, out, 5, plain, len + 1, out + 5, out + 5 + len + 1);
    t.wr.seq++;
    return tcp_send(out, 5 + clen);
}

/* ---------- powitanie ---------- */

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static unsigned client_hello(uint8_t *b, const char *host, const uint8_t pub[32])
{
    static const uint8_t sigalgs[] = {
        0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01,
        0x08, 0x06, 0x06, 0x01, 0x08, 0x07,
    };
    unsigned n = 4;                      /* miejsce na typ i dlugosc */
    b[n++] = 3; b[n++] = 3;              /* "wersja 1.2" - prawdziwa jest w rozszerzeniu */
    random_bytes(b + n, 32);             /* client random */
    n += 32;
    b[n++] = 32;                         /* session id (dla zgodnosci ze starymi posrednikami) */
    random_bytes(b + n, 32);
    n += 32;
    put16(b + n, 4); n += 2;             /* szyfry: ChaCha20 (szybszy bez sprzetowego AES), AES-GCM */
    put16(b + n, SUITE_CHACHA); n += 2;
    put16(b + n, SUITE_AES128GCM); n += 2;
    b[n++] = 1; b[n++] = 0;              /* kompresja: brak */

    unsigned ext_start = n;
    n += 2;
    unsigned hl = (unsigned)strlen(host);
    /* server_name - ktora strone chcemy (jeden serwer ma wiele stron) */
    put16(b + n, 0x0000); put16(b + n + 2, hl + 5); put16(b + n + 4, hl + 3);
    b[n + 6] = 0; put16(b + n + 7, hl); memcpy(b + n + 9, host, hl);
    n += 9 + hl;
    /* supported_groups: X25519 */
    put16(b + n, 0x000a); put16(b + n + 2, 4); put16(b + n + 4, 2); put16(b + n + 6, 0x001d);
    n += 8;
    /* signature_algorithms */
    put16(b + n, 0x000d); put16(b + n + 2, sizeof(sigalgs) + 2); put16(b + n + 4, sizeof(sigalgs));
    memcpy(b + n + 6, sigalgs, sizeof(sigalgs));
    n += 6 + sizeof(sigalgs);
    /* supported_versions: TLS 1.3 */
    put16(b + n, 0x002b); put16(b + n + 2, 3); b[n + 4] = 2; put16(b + n + 5, 0x0304);
    n += 7;
    /* key_share: nasz klucz publiczny X25519 */
    put16(b + n, 0x0033); put16(b + n + 2, 38); put16(b + n + 4, 36); put16(b + n + 6, 0x001d);
    put16(b + n + 8, 32); memcpy(b + n + 10, pub, 32);
    n += 42;
    /* ALPN: mowimy po HTTP/1.1 */
    put16(b + n, 0x0010); put16(b + n + 2, 11); put16(b + n + 4, 9); b[n + 6] = 8;
    memcpy(b + n + 7, "http/1.1", 8);
    n += 15;
    put16(b + ext_start, n - ext_start - 2);

    b[0] = HS_CLIENT_HELLO;
    b[1] = (uint8_t)((n - 4) >> 16);
    b[2] = (uint8_t)((n - 4) >> 8);
    b[3] = (uint8_t)(n - 4);
    return n;
}

/* Z ServerHello: wybrany szyfr i klucz publiczny serwera */
static int parse_server_hello(const uint8_t *m, unsigned len, uint8_t server_pub[32])
{
    static const uint8_t hrr[32] = {     /* SHA-256("HelloRetryRequest") */
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    if (len < 38)
        return TLS_EPROTO;
    if (memcmp(m + 2, hrr, 32) == 0)
        return TLS_EGROUP;
    unsigned p = 34;
    unsigned sid = m[p++];
    p += sid;
    if (p + 5 > len)
        return TLS_EPROTO;
    t.suite = be16(m + p);
    p += 3;                              /* szyfr + kompresja */
    if (t.suite != SUITE_CHACHA && t.suite != SUITE_AES128GCM)
        return TLS_ECIPHER;
    t.key_len = t.suite == SUITE_CHACHA ? 32 : 16;
    unsigned ext_end = p + 2 + be16(m + p);
    p += 2;
    int version13 = 0, have_key = 0;
    while (p + 4 <= ext_end && ext_end <= len) {
        unsigned type = be16(m + p), elen = be16(m + p + 2);
        const uint8_t *e = m + p + 4;
        if (p + 4 + elen > ext_end)
            return TLS_EPROTO;
        if (type == 0x002b && elen == 2 && be16(e) == 0x0304)
            version13 = 1;
        if (type == 0x0033 && elen >= 36 && be16(e) == 0x001d && be16(e + 2) == 32) {
            memcpy(server_pub, e + 4, 32);
            have_key = 1;
        }
        p += 4 + elen;
    }
    if (!version13)
        return TLS_EVERSION;
    return have_key ? 0 : TLS_EGROUP;
}

void tls_close(void)
{
    if (t.active && !t.peer_closed) {
        uint8_t close_notify[2] = { 1, 0 };           /* "koncze" */
        write_record(REC_ALERT, close_notify, 2);
    }
    kfree(t.hsbuf);
    kfree(t.app);
    memset(&t, 0, sizeof(t));                         /* klucze nie zostaja w pamieci */
}

int tls_connect(const char *host)
{
    memset(&t, 0, sizeof(t));
    t.timeout = 10000;
    t.hsbuf = kmalloc(HSBUF_MAX);
    t.app = kmalloc(REC_MAX);
    if (!t.hsbuf || !t.app) {
        tls_close();
        return NET_EDOWN;
    }
    random_mix((uint64_t)(uintptr_t)host);

    /* 1. ClientHello z nasza polowa klucza */
    uint8_t priv[32], pub[32], server_pub[32], shared[32];
    random_bytes(priv, 32);
    x25519_public(pub, priv);
    static uint8_t hello[512];
    unsigned hlen = client_hello(hello, host, pub);
    sha256_init(&t.transcript);
    sha256_update(&t.transcript, hello, hlen);
    int err = write_record(REC_HANDSHAKE, hello, hlen);

    /* 2. ServerHello (jawny) */
    uint8_t type, *data;
    unsigned len;
    while (!err) {
        err = read_record(&type, &data, &len);
        if (err)
            break;
        if (type == REC_ALERT) {
            t.alert = len >= 2 ? data[1] : 0;
            err = TLS_EALERT;
            break;
        }
        if (type != REC_HANDSHAKE || len < 4 || data[0] != HS_SERVER_HELLO)
            continue;
        unsigned mlen = be24(data + 1);
        if (mlen + 4 > len) {
            err = TLS_EPROTO;
            break;
        }
        err = parse_server_hello(data + 4, mlen, server_pub);
        if (!err)
            sha256_update(&t.transcript, data, mlen + 4);
        /* ServerHello i zaszyfrowane wiadomosci moga przyjsc w jednym rekordzie
         * tylko w teorii - w praktyce kolejne sa w osobnych rekordach */
        break;
    }
    if (err) {
        memset(priv, 0, sizeof(priv));
        tls_close();
        return err;
    }

    /* 3. Wspolny sekret i klucze powitania */
    x25519(shared, priv, server_pub);
    memset(priv, 0, sizeof(priv));
    uint8_t zero[32] = { 0 }, empty[32], early[32], derived[32], hs[32], h[32], c_hs[32], s_hs[32];
    sha256("", 0, empty);
    hkdf_extract(0, 0, zero, 32, early);
    derive_secret(early, "derived", empty, derived);
    hkdf_extract(derived, 32, shared, 32, hs);
    memset(shared, 0, sizeof(shared));
    transcript_hash(h);
    derive_secret(hs, "c hs traffic", h, c_hs);
    derive_secret(hs, "s hs traffic", h, s_hs);
    set_keys(&t.rd, s_hs);
    set_keys(&t.wr, c_hs);
    t.active = 1;

    /* 4. Zaszyfrowane wiadomosci serwera - az do Finished */
    int cert_requested = 0, done = 0;
    uint8_t cert_ctx[256];
    unsigned cert_ctx_len = 0;
    t.hs_len = 0;
    while (!done && !err) {
        err = read_record(&type, &data, &len);
        if (err)
            break;
        if (type == REC_CCS)
            continue;                    /* "zmiana szyfru" dla zgodnosci - ignorujemy */
        if (type == REC_ALERT) {
            t.alert = len >= 2 ? data[1] : 0;
            err = TLS_EALERT;
            break;
        }
        if (type != REC_HANDSHAKE || t.hs_len + len > HSBUF_MAX) {
            err = TLS_EPROTO;
            break;
        }
        memcpy(t.hsbuf + t.hs_len, data, len);
        t.hs_len += len;
        /* Wiadomosci moga byc podzielone miedzy rekordy albo lezec po kilka w jednym */
        while (t.hs_len >= 4 && !done && !err) {
            unsigned mlen = be24(t.hsbuf + 1);
            if (t.hs_len < mlen + 4)
                break;
            uint8_t mtype = t.hsbuf[0];
            if (mtype == HS_FINISHED) {
                uint8_t fkey[32], expect[32];
                expand_label(s_hs, "finished", 0, 0, fkey, 32);
                transcript_hash(h);
                hmac_sha256(fkey, 32, h, 32, expect);
                if (mlen != 32 || memcmp(expect, t.hsbuf + 4, 32) != 0)
                    err = TLS_EDECRYPT;          /* serwer nie zna kluczy - ktos sie podszywa? */
                done = 1;
            } else if (mtype == HS_CERT_REQUEST && mlen >= 1) {   /* kontekst ma najwyzej 255 B */
                cert_requested = 1;
                cert_ctx_len = t.hsbuf[4];
                memcpy(cert_ctx, t.hsbuf + 5, cert_ctx_len);
            }
            sha256_update(&t.transcript, t.hsbuf, mlen + 4);
            memmove(t.hsbuf, t.hsbuf + mlen + 4, t.hs_len - mlen - 4);
            t.hs_len -= mlen + 4;
        }
    }
    if (err) {
        tls_close();
        return err;
    }

    /* 5. Klucze do danych (z zapisu rozmowy az do Finished serwera) */
    uint8_t master[32], c_ap[32], s_ap[32];
    transcript_hash(h);
    derive_secret(hs, "derived", empty, derived);
    hkdf_extract(derived, 32, zero, 32, master);
    derive_secret(master, "c ap traffic", h, c_ap);
    derive_secret(master, "s ap traffic", h, s_ap);

    /* 6. Nasze Finished (i pusty certyfikat, gdyby serwer o niego prosil) */
    t.active = 0;
    static const uint8_t ccs = 1;
    err = write_record(REC_CCS, &ccs, 1);
    t.active = 1;
    if (!err && cert_requested) {
        uint8_t cert[4 + 1 + 255 + 3];
        unsigned n = 0;
        cert[n++] = HS_CERTIFICATE;
        unsigned body = 1 + cert_ctx_len + 3;
        cert[n++] = 0; cert[n++] = 0; cert[n++] = (uint8_t)body;
        cert[n++] = (uint8_t)cert_ctx_len;
        memcpy(cert + n, cert_ctx, cert_ctx_len);
        n += cert_ctx_len;
        cert[n++] = 0; cert[n++] = 0; cert[n++] = 0;     /* lista certyfikatow: pusta */
        sha256_update(&t.transcript, cert, n);
        err = write_record(REC_HANDSHAKE, cert, n);
    }
    if (!err) {
        uint8_t fkey[32], fin[4 + 32];
        expand_label(c_hs, "finished", 0, 0, fkey, 32);
        transcript_hash(h);
        fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
        hmac_sha256(fkey, 32, h, 32, fin + 4);
        err = write_record(REC_HANDSHAKE, fin, sizeof(fin));
    }
    if (err) {
        tls_close();
        return err;
    }
    set_keys(&t.rd, s_ap);
    set_keys(&t.wr, c_ap);
    t.timeout = 10000;
    return 0;
}

/* ---------- dane ---------- */

int tls_send(const void *data, unsigned len)
{
    const uint8_t *p = data;
    while (len) {
        unsigned n = len < 16384 ? len : 16384;
        int err = write_record(REC_APPDATA, p, n);
        if (err)
            return err;
        p += n;
        len -= n;
    }
    return 0;
}

int tls_recv(void *buf, unsigned size, uint32_t timeout_ms)
{
    t.timeout = timeout_ms;
    while (t.app_pos >= t.app_len) {
        if (t.peer_closed)
            return 0;
        uint8_t type, *data;
        unsigned len;
        int err = read_record(&type, &data, &len);
        if (err == NET_ERESET)
            return 0;                    /* serwer zamknal TCP bez close_notify - traktujemy jak koniec */
        if (err)
            return err;
        if (type == REC_APPDATA) {
            memcpy(t.app, data, len);
            t.app_len = len;
            t.app_pos = 0;
        } else if (type == REC_ALERT) {
            t.peer_closed = 1;           /* close_notify albo blad - koniec */
            if (len >= 2 && data[1] != 0) {
                t.alert = data[1];
                return TLS_EALERT;
            }
        } else if (type == REC_HANDSHAKE && len >= 4 && data[0] == HS_KEY_UPDATE) {
            uint8_t next[32];            /* serwer zmienia klucz - liczymy nowy */
            expand_label(t.rd.secret, "traffic upd", 0, 0, next, 32);
            set_keys(&t.rd, next);
        }
        /* NewSessionTicket (bilet do wznowienia polaczenia) - nie uzywamy */
    }
    unsigned n = t.app_len - t.app_pos < size ? t.app_len - t.app_pos : size;
    memcpy(buf, t.app + t.app_pos, n);
    t.app_pos += n;
    return (int)n;
}
