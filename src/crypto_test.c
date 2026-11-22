/* Komenda "kryptotest": liczy SHA-256, HMAC, HKDF, X25519, ChaCha20-Poly1305
 * i AES-128-GCM na ustalonych danych i wypisuje wyniki szesnastkowo - mozna
 * je porownac z wzorcowa implementacja (albo z wektorami z dokumentow RFC). */

#include "crypto.h"
#include "vga.h"
#include "string.h"

static void hex(const char *name, const uint8_t *p, size_t n)
{
    kprintf("%s ", name);
    for (size_t i = 0; i < n; i++)
        kprintf("%02x", p[i]);
    vga_putc('\n');
}

void cmd_kryptotest(char *args)
{
    (void)args;
    uint8_t out[64], tag[16], back[128];

    sha256("abc", 3, out);
    hex("SHA", out, 32);

    static const char fox[] = "The quick brown fox jumps over the lazy dog";
    hmac_sha256((const uint8_t *)"key", 3, fox, sizeof(fox) - 1, out);
    hex("HMAC", out, 32);

    uint8_t ikm[22], salt[13], info[10], prk[32];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    hkdf_expand(prk, info, sizeof(info), out, 42);
    hex("HKDF", out, 42);

    uint8_t scalar[32], point[32];
    for (int i = 0; i < 32; i++) {
        scalar[i] = (uint8_t)(i * 7 + 1);
        point[i] = (uint8_t)(i * 13 + 5);
    }
    point[31] &= 0x7f;
    x25519(out, scalar, point);
    hex("X25519", out, 32);
    x25519_public(out, scalar);
    hex("X25519PUB", out, 32);

    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0x40 + i);
    static const char msg[] = "NaszOS szyfruje! Ten tekst ma wiecej niz 64 bajty, zeby przejsc przez dwa bloki.";
    uint8_t ct[128];
    size_t len = sizeof(msg) - 1;
    chacha20poly1305_encrypt(key, nonce, (const uint8_t *)"aad", 3, (const uint8_t *)msg, len, ct, tag);
    hex("CHACHA", ct, 16);
    hex("CHACHATAG", tag, 16);
    int ok = chacha20poly1305_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, len, back, tag) == 0 &&
             memcmp(back, msg, len) == 0;
    ct[5] ^= 1;
    int tamper = chacha20poly1305_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, len, back, tag) != 0;
    kprintf("CHACHAROUND %s %s\n", ok ? "ok" : "ZLE", tamper ? "ok" : "ZLE");

    aes128gcm_encrypt(key, nonce, (const uint8_t *)"aad", 3, (const uint8_t *)msg, len, ct, tag);
    hex("GCM", ct, 16);
    hex("GCMTAG", tag, 16);
    ok = aes128gcm_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, len, back, tag) == 0 &&
         memcmp(back, msg, len) == 0;
    ct[5] ^= 1;
    tamper = aes128gcm_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, len, back, tag) != 0;
    kprintf("GCMROUND %s %s\n", ok ? "ok" : "ZLE", tamper ? "ok" : "ZLE");
}
