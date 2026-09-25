// crypto_secret.c first: every other file wipes through it. The rest depend only on it and on monocypher.
#include "nyangine-core/crypto/crypto_secret.c"
/**/
#include "nyangine-core/crypto/crypto_aead.c"
#include "nyangine-core/crypto/crypto_encoding.c"
// after crypto_aead.c and crypto_encoding.c, the two it composes into a sealed base64url box.
#include "nyangine-core/crypto/crypto_seal.c"
#include "nyangine-core/crypto/crypto_exchange.c"
#include "nyangine-core/crypto/crypto_hash.c"
#include "nyangine-core/crypto/crypto_kdf.c"
#include "nyangine-core/crypto/crypto_ecdsa.c"
#include "nyangine-core/crypto/crypto_rsa.c"
#include "nyangine-core/crypto/crypto_sign.c"
// after crypto_hash.c, whose HMAC-SHA1 it is a thin layer of arithmetic over.
#include "nyangine-core/crypto/crypto_totp.c"
