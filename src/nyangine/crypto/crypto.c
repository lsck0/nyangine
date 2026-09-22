// crypto_secret.c first: every other file wipes through it. The rest depend only on it and on monocypher.
#include "nyangine/crypto/crypto_secret.c"
/**/
#include "nyangine/crypto/crypto_aead.c"
#include "nyangine/crypto/crypto_encoding.c"
#include "nyangine/crypto/crypto_exchange.c"
#include "nyangine/crypto/crypto_hash.c"
#include "nyangine/crypto/crypto_kdf.c"
#include "nyangine/crypto/crypto_sign.c"
// after crypto_hash.c, whose HMAC-SHA1 it is a thin layer of arithmetic over.
#include "nyangine/crypto/crypto_totp.c"
