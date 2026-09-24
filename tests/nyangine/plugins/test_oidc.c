/**
 * "Log in with Google" and its relatives, driven with no network: a canned reply per call and a clock
 * this file moves.
 *
 * What is proved here is the part that cannot be proved against a real provider in CI — that a
 * discovery document naming the wrong issuer is refused, that an authorize url carries a real PKCE
 * challenge, and that an id_token failing any one check is refused whole, each with its own message.
 *
 * The RSA fixtures below are a real 2048 bit key, generated once with:
 *   openssl genrsa -out key.pem 2048
 * and never used again outside this file. The id_tokens are real too: each is signed offline with that
 * key over exactly the header and payload this file embeds, the same way `openssl dgst -sha256 -sign
 * key.pem` would sign them, so nya_crypto_rsa_verify_sha256 is exercised for real rather than stubbed.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Replies the fake holds before a test has to drain it. */
#define SCRIPT_MAX 8

#define ISSUER        "https://issuer.test"
#define CLIENT_ID     "client-123"
#define CLIENT_SECRET "test-client-secret-fixture"
#define REDIRECT_URI  "https://example.test/callback"
#define SCOPES        "openid email profile"

/* RSA FIXTURE — see the file note above. */

#define JWKS_N                                                                                                                                       \
  "rNTZZodETUlBmRuqVoYznjyGvO3tfzAPv3JoVT2eZ1BMhFEsMpSfBt2fzERbpCW_21bGFav879ghV4Gcwdzmo1jhyL05AJbpgfnps2OT2qGZymWy89BseBrAPuHEghyO1veMu8MzMZE8fBisN" \
  "TaRyXojkt2TWvPCwYY7tRjKzHOFqYEjux7raXjiF2PP0a3ilnelmcrplwg-3JDfGCCtHpzWvU8xpFreZk-YW7QNag5cyQyXmVkblccaP2UYEqrWitxLzzUsHf4Cl0pZsduqv8VL1QG_qCJ1Kp" \
  "ROdeiUIw-K6Xln8gSgM7HsosAwTfAwHqM4p40jNHPZgPXJRyTVlQ"
#define JWKS_E "AQAB"

/** alg RS256, kid test-key-1, aud client-123, sub user-42, nonce "test-nonce-fixture", exp/iat both sane. */
#define ID_TOKEN_GOOD                                                                                                                                 \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "fq_iVGSzUEdZGY3me3MtPAxw3MRC4IEshF7r5iRB2ZJmtTxnXBRzpxCYvzUrDI9v_qKW5W1nsmlhfsi7G36u4uq2Fnh9M5LW9UaEXLwzzqRWqNesrU-3p5lYfXCjtpuXxzMKJ3Q0pEODCfro"   \
  "ZVbj5LljsIQiDqrCmcRXkqdCOatG3cutQjeCmE-g9wi7z1kOMD6Ks-2NZPmQQq8GZDq0yYSocWRyftt_0C2JZEBpdZdBxNBvf6-FC6d0xvSoAos7Fw71knhMvO-TgJvfv9IPYYtrnDO_yTTb"   \
  "UwXMMj13zOfptNpvmyovfEFD4-I6KJgRtPceIP-6stO7VzsVcgskcA"

/** Same claims, `aud` is a client this test never configured. */
#define ID_TOKEN_WRONG_AUD                                                                                                                            \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoic29tZW9uZS1lbHNlIiwic3ViIjoidXNlci00MiIsImV4cCI6MTcwMDAwMzYwMCwiaWF0IjoxNjk5OTk5OTkwLCJub25jZSI6"   \
  "InRlc3Qtbm9uY2UtZml4dHVyZSIsImVtYWlsIjoiYWRhQGV4YW1wbGUudGVzdCIsImVtYWlsX3ZlcmlmaWVkIjp0cnVlLCJuYW1lIjoiQWRhIEZpeHR1cmUiLCJwaWN0dXJlIjoiaHR0cHM6"   \
  "Ly9pc3N1ZXIudGVzdC9hZGEucG5nIn0."                                                                                                                   \
  "QlXB1BgvPs1wO85ubnninE7FIzp8QNVxyU8W3DXF_NY0dEsCMw6YLJgZQ85jJ5SPH5utAhYgmGXJtRdzrYmcm_nWHO258gY5XjYU3nxwewmSgJbDkhB6tGgtPvN6CfZ_WvjS9SAzTnhkHGbp"   \
  "rU94iPIMurlHge7-q3AZWCJd0rhwUG2GyigKF1nAk66m96DNwi2bs82-tAzHp2fcQ5fqUyl7dev0OPw5I-vNtmt3MgLSzM8yxg9SChJ1wr2gDONjPbAwFzTEcLnV5EM5nuj-mopIQ2Hp-b77"  \
  "nnzL5_2XIfqDgsRKnQ76eAIzs_tlYkHOzAP3X_82I52-xYvcAEpzyQ"

/** Same claims, `exp` and `iat` both well before the clock this file uses. */
#define ID_TOKEN_EXPIRED                                                                                                                              \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE2OTk5OTkwMDAsImlhdCI6MTY5OTk5NTAwMCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "oTXDAre3lzW2egQ9qfCl08dj4dLcO9bgaA9Vw2pRgKS_6wnul3wkHvng7f7WnR8HxPVv3I2yXXObBmaIFrbjx2y_jFtNCTkqrc8ehfxknNRVlK9X4ORMBq0cSiWyAmCo9EWod8ipECXXPwi4"   \
  "6AKmxZVQequaY7WD3UHQkzCFCzrZ3fzyFE3pCoQQLQYRciZLhyrkc_IBC4xaoKFEbLdqBHjN8okyTm9cXtI3MB_56SdLqD5CTTK5tN6cxgjVFuUnmoMPqbepN0326_YmVHix_eRfRtkeS6hK"   \
  "Yiwk3ZFpcYDRjLf3gsJ9CJBgvkSvGCcrHA6u05V_9n_zF_6Q7rTcfg"

/** Same claims, `nonce` is not what this login started with. */
#define ID_TOKEN_WRONG_NONCE                                                                                                                          \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJh"   \
  "LWRpZmZlcmVudC1ub25jZSIsImVtYWlsIjoiYWRhQGV4YW1wbGUudGVzdCIsImVtYWlsX3ZlcmlmaWVkIjp0cnVlLCJuYW1lIjoiQWRhIEZpeHR1cmUiLCJwaWN0dXJlIjoiaHR0cHM6Ly9p"   \
  "c3N1ZXIudGVzdC9hZGEucG5nIn0."                                                                                                                       \
  "CeD7L5f4C-2OG8KNQ7Le9ZmPiWasi1Y1wkEunV6Wa3mVR4wGIsas5BVtTadMNhOkRwLXKk8tKfktTBtOgb7o87VB9o5pwNNy3f3UzuUWpM4k_VFNGFtbxYFgpoIZ1-r6hU0e0SBHO-KuHX3r"   \
  "5KPGNeU0kvZRhjRmrpNyXfcWglhQWPGhz2KYPg112w7QF5YTE3CbzbLrALXa8PQGsjDaBGlKYZU4eHfcvaRa8cSVURoU1Y5jnu_6XWp4QO2zZXktta4PrXue-La60-MY4o5HHhd39kbxOqG"   \
  "p34A-6JkSqueIxa6x-jFn8N0w_K3OHLvMtNSpNSv8wrlu66HGMb-hxA"

/** ID_TOKEN_GOOD's own signature with its last byte flipped: same claims, does not verify. */
#define ID_TOKEN_BAD_SIGNATURE                                                                                                                        \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "fq_iVGSzUEdZGY3me3MtPAxw3MRC4IEshF7r5iRB2ZJmtTxnXBRzpxCYvzUrDI9v_qKW5W1nsmlhfsi7G36u4uq2Fnh9M5LW9UaEXLwzzqRWqNesrU-3p5lYfXCjtpuXxzMKJ3Q0pEODCfro"   \
  "ZVbj5LljsIQiDqrCmcRXkqdCOatG3cutQjeCmE-g9wi7z1kOMD6Ks-2NZPmQQq8GZDq0yYSocWRyftt_0C2JZEBpdZdBxNBvf6-FC6d0xvSoAos7Fw71knhMvO-TgJvfv9IPYYtrnDO_yTTb"   \
  "UwXMMj13zOfptNpvmyovfEFD4-I6KJgRtPceIP-6stO7VzsVcgskjw"

/** header {"alg":"none","typ":"JWT"}; payload and signature from ID_TOKEN_GOOD, never reached. */
#define ID_TOKEN_ALG_NONE                                                                                                                             \
  "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."                                                                                                              \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "fq_iVGSzUEdZGY3me3MtPAxw3MRC4IEshF7r5iRB2ZJmtTxnXBRzpxCYvzUrDI9v_qKW5W1nsmlhfsi7G36u4uq2Fnh9M5LW9UaEXLwzzqRWqNesrU-3p5lYfXCjtpuXxzMKJ3Q0pEODCfro"   \
  "ZVbj5LljsIQiDqrCmcRXkqdCOatG3cutQjeCmE-g9wi7z1kOMD6Ks-2NZPmQQq8GZDq0yYSocWRyftt_0C2JZEBpdZdBxNBvf6-FC6d0xvSoAos7Fw71knhMvO-TgJvfv9IPYYtrnDO_yTTb"   \
  "UwXMMj13zOfptNpvmyovfEFD4-I6KJgRtPceIP-6stO7VzsVcgskcA"

/** header {"alg":"HS256","kid":"test-key-1","typ":"JWT"}: the RS256-to-HS256 downgrade. */
#define ID_TOKEN_ALG_HS256                                                                                                                            \
  "eyJhbGciOiJIUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."                                                                                   \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "fq_iVGSzUEdZGY3me3MtPAxw3MRC4IEshF7r5iRB2ZJmtTxnXBRzpxCYvzUrDI9v_qKW5W1nsmlhfsi7G36u4uq2Fnh9M5LW9UaEXLwzzqRWqNesrU-3p5lYfXCjtpuXxzMKJ3Q0pEODCfro"   \
  "ZVbj5LljsIQiDqrCmcRXkqdCOatG3cutQjeCmE-g9wi7z1kOMD6Ks-2NZPmQQq8GZDq0yYSocWRyftt_0C2JZEBpdZdBxNBvf6-FC6d0xvSoAos7Fw71knhMvO-TgJvfv9IPYYtrnDO_yTTb"   \
  "UwXMMj13zOfptNpvmyovfEFD4-I6KJgRtPceIP-6stO7VzsVcgskcA"

/** header names a kid this test's jwks never publishes; payload and signature from ID_TOKEN_GOOD, never reached. */
#define ID_TOKEN_UNKNOWN_KID                                                                                                                          \
  "eyJhbGciOiJSUzI1NiIsImtpZCI6Im5vdC1hLXJlYWwta2V5IiwidHlwIjoiSldUIn0."                                                                              \
  "eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIjoiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0"   \
  "ZXN0LW5vbmNlLWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIiwicGljdHVyZSI6Imh0dHBzOi8v"   \
  "aXNzdWVyLnRlc3QvYWRhLnBuZyJ9."                                                                                                                      \
  "fq_iVGSzUEdZGY3me3MtPAxw3MRC4IEshF7r5iRB2ZJmtTxnXBRzpxCYvzUrDI9v_qKW5W1nsmlhfsi7G36u4uq2Fnh9M5LW9UaEXLwzzqRWqNesrU-3p5lYfXCjtpuXxzMKJ3Q0pEODCfro"   \
  "ZVbj5LljsIQiDqrCmcRXkqdCOatG3cutQjeCmE-g9wi7z1kOMD6Ks-2NZPmQQq8GZDq0yYSocWRyftt_0C2JZEBpdZdBxNBvf6-FC6d0xvSoAos7Fw71knhMvO-TgJvfv9IPYYtrnDO_yTTb"   \
  "UwXMMj13zOfptNpvmyovfEFD4-I6KJgRtPceIP-6stO7VzsVcgskcA"

/* REPLIES */

#define JWKS_EC_X "5P_TGFB_fOb8s9HiuPd7FKMl10sDvGETdFrYcq8kkLo"
#define JWKS_EC_Y "bJdoN89iTGM4siIKrVoAdbhHhwYxLVyPGdSKpn4wURU"

/** alg ES256, kid test-key-ec, the same claims the RSA fixture carries. */
#define ID_TOKEN_ES256 \
  "eyJhbGciOiJFUzI1NiIsImtpZCI6InRlc3Qta2V5LWVjIiwidHlwIjoiSldUIn0.eyJpc3MiOiJodHRwczovL2lzc3Vlci50ZXN0IiwiYXVkIj"\
  "oiY2xpZW50LTEyMyIsInN1YiI6InVzZXItNDIiLCJleHAiOjE3MDAwMDM2MDAsImlhdCI6MTY5OTk5OTk5MCwibm9uY2UiOiJ0ZXN0LW5vbmNl"\
  "LWZpeHR1cmUiLCJlbWFpbCI6ImFkYUBleGFtcGxlLnRlc3QiLCJlbWFpbF92ZXJpZmllZCI6dHJ1ZSwibmFtZSI6IkFkYSBGaXh0dXJlIn0.3N"\
  "gMU8U46jg3OapyY5IcVKINgikvpSMIR0Krlp1h4og2boS2FFuT2jYbNkznJ4Vbgqeg3Olp11qTCDOQ8lSRRw"

static const char* DISCOVERY_GOOD = "{\"issuer\":\"" ISSUER "\",\"authorization_endpoint\":\"" ISSUER "/authorize\","
                                     "\"token_endpoint\":\"" ISSUER "/token\",\"jwks_uri\":\"" ISSUER "/jwks\","
                                     "\"userinfo_endpoint\":\"" ISSUER "/userinfo\"}";

static const char* DISCOVERY_WRONG_ISSUER = "{\"issuer\":\"https://not-the-issuer.test\",\"authorization_endpoint\":\"" ISSUER "/authorize\","
                                             "\"token_endpoint\":\"" ISSUER "/token\",\"jwks_uri\":\"" ISSUER "/jwks\"}";

static const char* JWKS_GOOD = "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"test-key-1\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"" JWKS_N
                                "\",\"e\":\"" JWKS_E "\"}]}";

/** Both keys, which is what a provider that signs some tokens with each publishes. */
static const char* JWKS_RSA_AND_EC = "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"test-key-1\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"" JWKS_N
                                      "\",\"e\":\"" JWKS_E "\"},"
                                      "{\"kty\":\"EC\",\"kid\":\"test-key-ec\",\"use\":\"sig\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"x\":\"" JWKS_EC_X
                                      "\",\"y\":\"" JWKS_EC_Y "\"}]}";

/** The EC key published under the *RSA* key's kid: the confusion the family check refuses. */
static const char* JWKS_EC_AS_RSA_KID = "{\"keys\":[{\"kty\":\"EC\",\"kid\":\"test-key-1\",\"use\":\"sig\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"x\":\"" JWKS_EC_X
                                         "\",\"y\":\"" JWKS_EC_Y "\"}]}";

#define TOKEN_BODY(id_token)                                                                                                                         \
  "{\"access_token\":\"test-access-token-fixture\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"id_token\":\"" id_token "\"}"

/* THE FAKE */

typedef struct {
  u32         status;
  const char* body;
} Reply;

typedef struct {
  Reply script[SCRIPT_MAX];
  u32   script_count;
  u32   script_read;

  char last_url[512];
  char last_body[2048];
  u32  performed;

  u64 now_ms;
  u64 now_s;
} Fake;

static NYA_Error fake_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
  Fake* fake = (Fake*)user;

  (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", request.url);

  // kept across a later bodyless call (the jwks GET that can follow a token POST inside one exchange),
  // so a test can still see what the POST carried after nya_oidc_exchange returns.
  if (request.body != nullptr) {
    NYA_String* serialized = nya_serde_json_serialize(arena, request.body, NYA_SERDE_NONE);
    (void)snprintf(fake->last_body, sizeof(fake->last_body), "%s", nya_string_to_cstring(arena, serialized));
  }

  fake->performed += 1;

  nya_assert(fake->script_read < fake->script_count, "the fake ran out of scripted replies");
  const Reply* reply = &fake->script[fake->script_read++];

  *out_response = (NYA_Response){
    .status      = reply->status,
    .raw_body    = nya_string_from(arena, reply->body != nullptr ? reply->body : ""),
    .raw_headers = nya_string_from(arena, ""),
  };

  if (out_response->raw_body->length > 0) {
    NYA_Object* parsed = nullptr;
    if (nya_serde_json_deserialize(arena, out_response->raw_body->items, out_response->raw_body->length, NYA_SERDE_NONE, &parsed).ok) {
      out_response->body = parsed;
    }
  }

  return nya_request_status_is_success(reply->status) ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "request returned %u", reply->status);
}

static u64 fake_now_ms(void* user) {
  return ((Fake*)user)->now_ms;
}

static u64 fake_now_s(void* user) {
  return ((Fake*)user)->now_s;
}

static void fake_push(Fake* fake, u32 status, const char* body) {
  nya_assert(fake->script_count < SCRIPT_MAX);

  fake->script[fake->script_count++] = (Reply){ .status = status, .body = body };
}

static NYA_OidcOptions fake_options(Fake* fake) {
  return (NYA_OidcOptions){
    .issuer        = ISSUER,
    .client_id     = CLIENT_ID,
    .client_secret = CLIENT_SECRET,
    .redirect_uri  = REDIRECT_URI,
    .scopes        = SCOPES,
    .perform       = fake_perform,
    .now_ms        = fake_now_ms,
    .now_s         = fake_now_s,
    .user          = fake,
  };
}

/** A provider that has already discovered against a fresh Fake, for tests that only care about what comes after. */
static NYA_OidcProvider* discovered_provider(NYA_Arena* arena, Fake* fake) {
  fake_push(fake, 200, DISCOVERY_GOOD);

  NYA_OidcProvider* provider = nullptr;
  nya_check(nya_oidc_create(arena, fake_options(fake), &provider).ok, "the provider is made");
  nya_check(nya_oidc_discover(provider, arena).ok, "discovery succeeds");

  return provider;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_oidc");
  defer      nya_arena_destroy(arena);

  // TEST: a discovery document naming a different issuer is refused, and nothing from it is kept.
  {
    Fake fake = { .now_ms = 1000, .now_s = 1'700'000'000 };
    fake_push(&fake, 200, DISCOVERY_WRONG_ISSUER);

    NYA_OidcProvider* provider = nullptr;
    nya_check(nya_oidc_create(arena, fake_options(&fake), &provider).ok, "the provider is made");

    NYA_Error discovered = nya_oidc_discover(provider, arena);
    nya_check(!discovered.ok, "a mismatched issuer is refused");
    nya_check(nya_string_contains((NYA_ConstCString)discovered.message, "issuer"), "the message says why: %s", (NYA_ConstCString)discovered.message);

    // the authorize url needs discovery to have succeeded, which this one never did.
    char                   url[1024]  = { 0 };
    NYA_OidcAuthorizeState out_state = { 0 };
    nya_check(!nya_oidc_authorize_url(provider, url, sizeof(url), &out_state).ok, "and nothing works off a refused discovery");
  }

  // TEST: the authorize url carries state, nonce and a real S256 PKCE challenge.
  {
    Fake              fake     = { .now_ms = 1000, .now_s = 1'700'000'000 };
    NYA_OidcProvider* provider = discovered_provider(arena, &fake);

    char                   url[1024] = { 0 };
    NYA_OidcAuthorizeState state     = { 0 };
    nya_check(nya_oidc_authorize_url(provider, url, sizeof(url), &state).ok, "an authorize url is built");

    nya_check(nya_string_contains((NYA_ConstCString)url, ISSUER "/authorize?"), "against the discovered endpoint, got '%s'", url);
    nya_check(nya_string_contains((NYA_ConstCString)url, "response_type=code"), "asking for a code");
    nya_check(nya_string_contains((NYA_ConstCString)url, "code_challenge_method=S256"), "with an S256 challenge");

    nya_check(strlen(state.state) == 43, "state is a real random value, got %zu characters", strlen(state.state));
    nya_check(strlen(state.nonce) == 43, "nonce too, got %zu characters", strlen(state.nonce));
    nya_check(strlen(state.code_verifier) == 43, "and the PKCE verifier, got %zu characters", strlen(state.code_verifier));
    nya_check(!nya_string_equals((NYA_ConstCString)state.state, (NYA_ConstCString)state.nonce), "state and nonce are not the same draw");

    char query_state[64] = { 0 };
    (void)snprintf(query_state, sizeof(query_state), "state=%s", state.state);
    nya_check(nya_string_contains((NYA_ConstCString)url, query_state), "the url carries this exact state");

    char query_nonce[64] = { 0 };
    (void)snprintf(query_nonce, sizeof(query_nonce), "nonce=%s", state.nonce);
    nya_check(nya_string_contains((NYA_ConstCString)url, query_nonce), "and this exact nonce");

    // the challenge in the url is base64url(sha256(verifier)), checked against this file's own crypto
    // rather than trusted because the field is present.
    NYA_CryptoSha256Digest expected_hash = { 0 };
    nya_crypto_sha256((const u8*)state.code_verifier, strlen(state.code_verifier), &expected_hash);

    char expected_challenge[64] = { 0 };
    u64  expected_length         = 0;
    // nya_crypto_base64url_encode rather than a crypto_encoding.h call: this file shares oidc.c's
    // translation unit, and that is the temporary local copy oidc.c itself uses — see its file note.
    nya_check(
        nya_crypto_base64url_encode(expected_hash.bytes, sizeof(expected_hash.bytes), expected_challenge, sizeof(expected_challenge), &expected_length),
        "the expected challenge encodes"
    );

    char query_challenge[128] = { 0 };
    (void)snprintf(query_challenge, sizeof(query_challenge), "code_challenge=%s&", expected_challenge);
    nya_check(nya_string_contains((NYA_ConstCString)url, query_challenge), "and the challenge really is S256 of the verifier, got '%s'", url);
  }

  // TEST: a good exchange verifies the id_token and hands back its claims.
  {
    Fake              fake     = { .now_ms = 1000, .now_s = 1'700'000'000 };
    NYA_OidcProvider* provider = discovered_provider(arena, &fake);

    fake_push(&fake, 200, TOKEN_BODY(ID_TOKEN_GOOD));
    fake_push(&fake, 200, JWKS_GOOD);

    NYA_OidcAuthorizeState state = { 0 };
    (void)snprintf(state.state, sizeof(state.state), "%s", "whatever-the-caller-kept");
    (void)snprintf(state.nonce, sizeof(state.nonce), "%s", "test-nonce-fixture");
    (void)snprintf(state.code_verifier, sizeof(state.code_verifier), "%s", "test-verifier-fixture");

    NYA_OidcClaims claims = { 0 };
    NYA_Error      result = nya_oidc_exchange(provider, arena, "test-authorization-code", &state, &claims);

    nya_check(result.ok, "the exchange succeeds: %s", (NYA_ConstCString)result.message);
    nya_check(nya_string_equals((NYA_ConstCString)claims.subject, "user-42"), "the subject came through, got '%s'", claims.subject);
    nya_check(nya_string_equals((NYA_ConstCString)claims.issuer, ISSUER), "the issuer too, got '%s'", claims.issuer);
    nya_check(nya_string_equals((NYA_ConstCString)claims.email, "ada@example.test"), "and the email, got '%s'", claims.email);
    nya_check(claims.email_verified, "email_verified read as true");
    nya_check(nya_string_equals((NYA_ConstCString)claims.name, "Ada Fixture"), "the name, got '%s'", claims.name);
    nya_check(nya_string_equals((NYA_ConstCString)claims.access_token, "test-access-token-fixture"), "and the access_token for userinfo, got '%s'",
              claims.access_token);
    nya_check(claims.raw != nullptr, "the raw payload is kept");

    NYA_Value* raw_sub = nya_object_get(claims.raw, "sub");
    nya_check(raw_sub != nullptr && nya_string_equals((NYA_ConstCString)raw_sub->as_string, "user-42"), "and it holds the same claims");

    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "test-verifier-fixture"), "the verifier went to the token endpoint, got '%s'",
              fake.last_body);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "test-authorization-code"), "the code too");

    // a second exchange with the same kid reuses the cached key rather than fetching the jwks again.
    fake_push(&fake, 200, TOKEN_BODY(ID_TOKEN_GOOD));

    NYA_OidcClaims second = { 0 };
    NYA_Error      again  = nya_oidc_exchange(provider, arena, "another-code", &state, &second);
    nya_check(again.ok, "the second exchange succeeds too: %s", (NYA_ConstCString)again.message);
    nya_check(fake.performed == 4, "with no extra jwks fetch, %u transfers so far", fake.performed);
  }

  // TEST: an ES256 token, against a provider that publishes both kinds of key.
  {
    Fake              fake     = { .now_ms = 1000, .now_s = 1'700'000'000 };
    NYA_OidcProvider* provider = discovered_provider(arena, &fake);

    fake_push(&fake, 200, TOKEN_BODY(ID_TOKEN_ES256));
    fake_push(&fake, 200, JWKS_RSA_AND_EC);

    NYA_OidcAuthorizeState state = { 0 };
    (void)snprintf(state.nonce, sizeof(state.nonce), "%s", "test-nonce-fixture");
    (void)snprintf(state.state, sizeof(state.state), "%s", "whatever");
    (void)snprintf(state.code_verifier, sizeof(state.code_verifier), "%s", "verifier");

    NYA_OidcClaims claims   = { 0 };
    NYA_Error      exchange = nya_oidc_exchange(provider, arena, "code", &state, &claims);

    nya_check(exchange.ok, "an ES256 id_token verifies: %s", (NYA_ConstCString)exchange.message);
    nya_check(nya_string_equals((NYA_ConstCString)claims.subject, "user-42"), "and carries its subject, got '%s'", claims.subject);

    /*
     * The same token against a jwks that publishes the EC key under the RSA key's kid. The signature
     * would verify with that key; what refuses it is that the header said ES256 and the kid it named
     * is not an ES256 key, which is the confusion a verifier keyed on kid alone would walk into.
     */
    Fake              confused          = { .now_ms = 1000, .now_s = 1'700'000'000 };
    NYA_OidcProvider* confused_provider = discovered_provider(arena, &confused);

    fake_push(&confused, 200, TOKEN_BODY(ID_TOKEN_GOOD));
    fake_push(&confused, 200, JWKS_EC_AS_RSA_KID);

    NYA_OidcClaims mismatched = { 0 };
    NYA_Error      refused    = nya_oidc_exchange(confused_provider, arena, "code", &state, &mismatched);

    nya_check(!refused.ok, "an RS256 token whose kid names an EC key is refused");
    nya_check(nya_string_contains((NYA_ConstCString)refused.message, "alg"), "saying the alg is not what the kid names, got '%s'",
              (NYA_ConstCString)refused.message);
  }

  // TEST: one refusal per broken check, each naming what broke.
  {
    struct {
      const char* name;
      const char* id_token;
      const char* needle;
      b8          needs_jwks;
    } cases[] = {
      { "wrong aud", ID_TOKEN_WRONG_AUD, "aud", true },
      { "expired", ID_TOKEN_EXPIRED, "expired", true },
      { "wrong nonce", ID_TOKEN_WRONG_NONCE, "nonce", true },
      { "alg none", ID_TOKEN_ALG_NONE, "alg", false },
      { "alg HS256 downgrade", ID_TOKEN_ALG_HS256, "alg", false },
      { "unknown kid", ID_TOKEN_UNKNOWN_KID, "kid", true },
      { "bad signature", ID_TOKEN_BAD_SIGNATURE, "signature", true },
    };

    for (u64 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Fake              fake     = { .now_ms = 1000, .now_s = 1'700'000'000 };
      NYA_OidcProvider* provider = discovered_provider(arena, &fake);

      char token_body[4096] = { 0 };
      (void)snprintf(
          token_body, sizeof(token_body),
          "{\"access_token\":\"test-access-token-fixture\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"id_token\":\"%s\"}", cases[i].id_token
      );
      fake_push(&fake, 200, token_body);
      if (cases[i].needs_jwks) fake_push(&fake, 200, JWKS_GOOD);

      NYA_OidcAuthorizeState state = { 0 };
      (void)snprintf(state.nonce, sizeof(state.nonce), "%s", "test-nonce-fixture");
      (void)snprintf(state.state, sizeof(state.state), "%s", "whatever");
      (void)snprintf(state.code_verifier, sizeof(state.code_verifier), "%s", "verifier");

      NYA_OidcClaims claims = { 0 };
      NYA_Error      result = nya_oidc_exchange(provider, arena, "code", &state, &claims);

      nya_check(!result.ok, "%s is refused", cases[i].name);
      nya_check(nya_string_contains((NYA_ConstCString)result.message, cases[i].needle), "%s: the message says '%s', got '%s'", cases[i].name,
                cases[i].needle, (NYA_ConstCString)result.message);
      nya_check(claims.subject[0] == '\0', "%s: nothing is half filled in", cases[i].name);
      nya_check(claims.raw == nullptr, "%s: no raw claims either", cases[i].name);
    }
  }

  // TEST: what a provider refuses before it ever sends anything.
  {
    Fake fake = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_OidcOptions options = fake_options(&fake);
    options.issuer          = nullptr;

    NYA_OidcProvider* refused = nullptr;
    nya_check(!nya_oidc_create(arena, options, &refused).ok, "a provider with no issuer is refused");

    options        = fake_options(&fake);
    options.scopes = "email profile";
    nya_check(!nya_oidc_create(arena, options, &refused).ok, "and one whose scopes never name 'openid'");

    options        = fake_options(&fake);
    options.scopes = "openidconnect";
    nya_check(!nya_oidc_create(arena, options, &refused).ok, "'openid' is a whole scope, not a prefix another one shares");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
