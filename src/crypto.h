#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* Kryptografia dla HTTPS (TLS 1.3) - src/crypto.c.
 *
 * UWAGA: to wersja edukacyjna, napisana "od zera" do nauki. Nie byla
 * sprawdzana przez specjalistow od bezpieczenstwa, a generator liczb
 * losowych NaszOS jest slaby - nie uzywaj jej do prawdziwych sekretow. */

/* ---- SHA-256: 32-bajtowy "odcisk palca" dowolnych danych ---- */
struct sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    unsigned n;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- HMAC i HKDF: "podpis" kluczem i wyprowadzanie kluczy ---- */
void hmac_sha256(const uint8_t *key, size_t key_len, const void *msg, size_t len, uint8_t out[32]);
void hkdf_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len, uint8_t *out, size_t len);

/* ---- X25519: uzgadnianie wspolnego sekretu (krzywa eliptyczna) ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t pub[32], const uint8_t priv[32]);

/* ---- Szyfry z pieczecia (AEAD): szyfruja i dopisuja 16-bajtowy znacznik,
 *      ktory wykrywa kazda zmiane. decrypt zwraca 0 = OK, -1 = zly znacznik ---- */
void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
int  chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16]);
void aes128gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
int  aes128gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16]);

/* ---- liczby losowe ---- */
void random_mix(uint64_t event);      /* dorzuc "szum" (np. czas zdarzenia) */
void random_bytes(void *out, size_t len);

#endif
