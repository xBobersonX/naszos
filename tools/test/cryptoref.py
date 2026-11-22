import hashlib, hmac
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305, AESGCM

print('SHA', hashlib.sha256(b'abc').hexdigest())
print('HMAC', hmac.new(b'key', b'The quick brown fox jumps over the lazy dog', hashlib.sha256).hexdigest())
ikm = b'\x0b' * 22; salt = bytes(range(13)); info = bytes(range(0xf0, 0xfa))
print('HKDF', HKDF(hashes.SHA256(), 42, salt, info).derive(ikm).hex())
scalar = bytes((i * 7 + 1) & 255 for i in range(32))
point = bytearray((i * 13 + 5) & 255 for i in range(32)); point[31] &= 0x7f
priv = X25519PrivateKey.from_private_bytes(scalar)
print('X25519', priv.exchange(X25519PublicKey.from_public_bytes(bytes(point))).hex())
from cryptography.hazmat.primitives import serialization
print('X25519PUB', priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex())
key = bytes(range(32)); nonce = bytes(range(0x40, 0x4c))
msg = b"NaszOS szyfruje! Ten tekst ma wiecej niz 64 bajty, zeby przejsc przez dwa bloki."
c = ChaCha20Poly1305(key).encrypt(nonce, msg, b'aad')
print('CHACHA', c[:16].hex()); print('CHACHATAG', c[-16:].hex())
g = AESGCM(key[:16]).encrypt(nonce, msg, b'aad')
print('GCM', g[:16].hex()); print('GCMTAG', g[-16:].hex())
