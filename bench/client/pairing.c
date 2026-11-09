#define _GNU_SOURCE
#include "bench.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIQUE_ID_LEN 16

static void random_bytes(unsigned char *out, size_t len) {
  RAND_bytes(out, (int)len);
}

static char *random_hex(size_t len) {
  unsigned char *raw = malloc(len);
  random_bytes(raw, len);
  char *hex = hex_encode(raw, len);
  free(raw);
  return hex;
}

/* Идентификатор клиента: 16 hex-символов, как в GFE. Живёт рядом с сертификатом. */
static const char *client_unique_id(const char *identity_dir) {
  static char uid[UNIQUE_ID_LEN * 2 + 1];
  char path[512];
  snprintf(path, sizeof(path), "%s/uniqueid", identity_dir);

  FILE *f = fopen(path, "r");
  if (f) {
    size_t n = fread(uid, 1, sizeof(uid) - 1, f);
    fclose(f);
    uid[n] = '\0';
    if (n == UNIQUE_ID_LEN * 2) {
      return uid;
    }
  }

  char *hex = random_hex(UNIQUE_ID_LEN);
  snprintf(uid, sizeof(uid), "%s", hex);
  free(hex);
  f = fopen(path, "w");
  if (f) {
    fputs(uid, f);
    fclose(f);
  }
  return uid;
}

/* Подпись сертификата (ASN.1) — moonlight использует её в хеше проверки */
static unsigned char *cert_signature_from_pem(const char *pem, size_t *out_len) {
  BIO *bio = BIO_new_mem_buf(pem, -1);
  if (!bio) {
    return NULL;
  }
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!cert) {
    return NULL;
  }
  const ASN1_BIT_STRING *sig = NULL;
  X509_get0_signature(&sig, NULL, cert);
  unsigned char *copy = NULL;
  if (sig) {
    *out_len = (size_t)ASN1_STRING_length(sig);
    copy = malloc(*out_len);
    memcpy(copy, ASN1_STRING_get0_data(sig), *out_len);
  }
  X509_free(cert);
  return copy;
}

static int verify_signature(const unsigned char *data, size_t data_len, const unsigned char *sig, size_t sig_len,
                            const char *cert_pem) {
  BIO *bio = BIO_new_mem_buf(cert_pem, -1);
  if (!bio) {
    return -1;
  }
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!cert) {
    return -1;
  }
  EVP_PKEY *pubkey = X509_get_pubkey(cert);
  X509_free(cert);
  if (!pubkey) {
    return -1;
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pubkey);
  EVP_DigestVerifyUpdate(ctx, data, data_len);
  int ok = EVP_DigestVerifyFinal(ctx, sig, sig_len);
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pubkey);
  return ok == 1 ? 0 : -1;
}

static int sign_message(const unsigned char *data, size_t data_len, EVP_PKEY *pkey,
                        unsigned char **out, size_t *out_len) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey);
  EVP_DigestSignUpdate(ctx, data, data_len);
  size_t len = 0;
  EVP_DigestSignFinal(ctx, NULL, &len);
  unsigned char *sig = malloc(len);
  EVP_DigestSignFinal(ctx, sig, &len);
  EVP_MD_CTX_free(ctx);
  *out = sig;
  *out_len = len;
  return 0;
}

static int paired_ok(const char *xml) {
  char *paired = xml_tag(xml, "paired");
  int ok = paired && strcmp(paired, "1") == 0;
  free(paired);
  return ok;
}

int pair_host(http_target *http, http_target *https, client_identity *id,
              const char *pin, const char *appversion, char **server_cert_pem) {
  const char *uid = client_unique_id(id->dir);
  unsigned char salt[16];
  random_bytes(salt, 16);
  char *salt_hex = hex_encode(salt, sizeof(salt));

  /* Ключ шифрования пейринга: первые 16 байт SHA-256(salt || PIN) */
  size_t pin_len = strlen(pin);
  unsigned char *salted = malloc(sizeof(salt) + pin_len);
  memcpy(salted, salt, sizeof(salt));
  memcpy(salted + sizeof(salt), pin, pin_len);
  unsigned char digest[32];
  sha256(salted, sizeof(salt) + pin_len, digest);
  free(salted);
  unsigned char aes_key[16];
  memcpy(aes_key, digest, 16);

  /* PEM клиентского сертификата в hex — так его ждёт wolf */
  FILE *f = fopen(id->cert_path, "r");
  if (!f) {
    bench_log("не удалось прочитать %s", id->cert_path);
    free(salt_hex);
    return -1;
  }
  char cert_pem[8192];
  size_t cert_len = fread(cert_pem, 1, sizeof(cert_pem) - 1, f);
  fclose(f);
  cert_pem[cert_len] = '\0';
  char *cert_hex = hex_encode((const unsigned char *)cert_pem, cert_len);

  int rc = -1;

  /* Фаза 1: получить сертификат сервера */
  char path[16384];
  snprintf(path, sizeof(path),
           "/pair?devicename=roth&updateState=1&phrase=getservercert&salt=%s&clientcert=%s&uniqueid=%s&uuid=%s",
           salt_hex, cert_hex, uid, random_hex(16));
  http_response r1 = http_get(http, path);
  if (r1.status != 200 || !paired_ok(r1.body)) {
    bench_log("фаза 1 не удалась: HTTP %ld", r1.status);
    http_response_free(&r1);
    goto out;
  }
  char *plaincert_hex = xml_tag(r1.body, "plaincert");
  http_response_free(&r1);
  if (!plaincert_hex) {
    bench_log("в ответе фазы 1 нет plaincert");
    goto out;
  }
  size_t pem_len = strlen(plaincert_hex) / 2;
  char *server_pem = malloc(pem_len + 1);
  if (hex_decode(plaincert_hex, (unsigned char *)server_pem, pem_len) != 0) {
    bench_log("plaincert не распарсился");
    free(plaincert_hex);
    free(server_pem);
    goto out;
  }
  server_pem[pem_len] = '\0';
  free(plaincert_hex);
  if (server_cert_pem) {
    *server_cert_pem = server_pem;
  }

  /* Сохраняем сертификат сервера: дальше по нему проверяем TLS-сессию
     (пин, как в настоящем Moonlight). */
  char pin_path[512];
  snprintf(pin_path, sizeof(pin_path), "%s/server.pem", id->dir);
  FILE *pin_file = fopen(pin_path, "w");
  if (pin_file) {
    fputs(server_pem, pin_file);
    fclose(pin_file);
  } else {
    bench_log("не удалось сохранить сертификат сервера в %s", pin_path);
  }

  /* Фаза 2: клиентский вызов */
  unsigned char client_challenge[16];
  random_bytes(client_challenge, sizeof(client_challenge));
  unsigned char enc_challenge[16];
  aes128_ecb_encrypt(aes_key, client_challenge, sizeof(client_challenge), enc_challenge);
  char *enc_challenge_hex = hex_encode(enc_challenge, sizeof(enc_challenge));

  snprintf(path, sizeof(path), "/pair?devicename=roth&updateState=1&clientchallenge=%s&uniqueid=%s&uuid=%s",
           enc_challenge_hex, uid, random_hex(16));
  free(enc_challenge_hex);
  http_response r2 = http_get(http, path);
  if (r2.status != 200 || !paired_ok(r2.body)) {
    bench_log("фаза 2 не удалась: HTTP %ld", r2.status);
    http_response_free(&r2);
    goto out;
  }
  char *challengeresponse_hex = xml_tag(r2.body, "challengeresponse");
  http_response_free(&r2);
  if (!challengeresponse_hex) {
    bench_log("в ответе фазы 2 нет challengeresponse");
    goto out;
  }
  size_t resp_len = strlen(challengeresponse_hex) / 2;
  unsigned char *resp_raw = malloc(resp_len);
  hex_decode(challengeresponse_hex, resp_raw, resp_len);
  free(challengeresponse_hex);
  unsigned char *resp_plain = malloc(resp_len);
  aes128_ecb_decrypt(aes_key, resp_raw, resp_len, resp_plain);
  free(resp_raw);
  if (resp_len < 32 + 16) {
    bench_log("challengeresponse слишком короткий (%zu)", resp_len);
    free(resp_plain);
    goto out;
  }
  unsigned char server_challenge[16];
  memcpy(server_challenge, resp_plain + 32, 16);

  /* Фаза 3: отклик на вызов сервера */
  unsigned char client_secret[16];
  random_bytes(client_secret, sizeof(client_secret));
  size_t challenge_resp_len = 16 + id->cert_signature_len + sizeof(client_secret);
  unsigned char *challenge_resp = malloc(challenge_resp_len);
  memcpy(challenge_resp, server_challenge, 16);
  memcpy(challenge_resp + 16, id->cert_signature, id->cert_signature_len);
  memcpy(challenge_resp + 16 + id->cert_signature_len, client_secret, sizeof(client_secret));
  unsigned char hash[32];
  sha256(challenge_resp, challenge_resp_len, hash);
  free(challenge_resp);
  unsigned char enc_hash[32];
  aes128_ecb_encrypt(aes_key, hash, sizeof(hash), enc_hash);
  char *enc_hash_hex = hex_encode(enc_hash, sizeof(enc_hash));

  snprintf(path, sizeof(path), "/pair?devicename=roth&updateState=1&serverchallengeresp=%s&uniqueid=%s&uuid=%s",
           enc_hash_hex, uid, random_hex(16));
  free(enc_hash_hex);
  http_response r3 = http_get(http, path);
  if (r3.status != 200 || !paired_ok(r3.body)) {
    bench_log("фаза 3 не удалась: HTTP %ld", r3.status);
    http_response_free(&r3);
    free(resp_plain);
    goto out;
  }
  char *pairing_secret_hex = xml_tag(r3.body, "pairingsecret");
  http_response_free(&r3);
  if (!pairing_secret_hex) {
    bench_log("в ответе фазы 3 нет pairingsecret");
    free(resp_plain);
    goto out;
  }
  size_t ps_len = strlen(pairing_secret_hex) / 2;
  unsigned char *ps = malloc(ps_len);
  hex_decode(pairing_secret_hex, ps, ps_len);
  free(pairing_secret_hex);
  if (ps_len < 16 + 32) {
    bench_log("pairingsecret слишком короткий");
    free(ps);
    free(resp_plain);
    goto out;
  }
  unsigned char server_secret[16];
  memcpy(server_secret, ps, 16);

  /* подпись сервера над server_secret */
  if (verify_signature(server_secret, sizeof(server_secret), ps + 16, ps_len - 16, server_pem) != 0) {
    bench_log("подпись сервера не сошлась (MITM?)");
    free(ps);
    free(resp_plain);
    goto out;
  }
  free(ps);

  /* проверка PIN: SHA-256(client_challenge || подпись сертификата сервера || server_secret) */
  size_t server_cert_sig_len = 0;
  unsigned char *server_cert_sig = cert_signature_from_pem(server_pem, &server_cert_sig_len);
  if (!server_cert_sig) {
    bench_log("не удалось получить подпись сертификата сервера");
    free(resp_plain);
    goto out;
  }
  size_t expected_len = sizeof(client_challenge) + server_cert_sig_len + sizeof(server_secret);
  unsigned char *expected = malloc(expected_len);
  memcpy(expected, client_challenge, sizeof(client_challenge));
  memcpy(expected + sizeof(client_challenge), server_cert_sig, server_cert_sig_len);
  memcpy(expected + sizeof(client_challenge) + server_cert_sig_len, server_secret, sizeof(server_secret));
  unsigned char expected_hash[32];
  sha256(expected, expected_len, expected_hash);
  free(expected);
  free(server_cert_sig);
  if (memcmp(expected_hash, resp_plain, 32) != 0) {
    bench_log("неверный PIN (хеш вызова не сошёлся)");
    free(resp_plain);
    goto out;
  }
  free(resp_plain);

  /* Фаза 4: клиентский секрет с подписью */
  unsigned char *client_sig = NULL;
  size_t client_sig_len = 0;
  sign_message(client_secret, sizeof(client_secret), id->pkey, &client_sig, &client_sig_len);
  size_t secret_len = sizeof(client_secret) + client_sig_len;
  unsigned char *client_pairing_secret = malloc(secret_len);
  memcpy(client_pairing_secret, client_secret, sizeof(client_secret));
  memcpy(client_pairing_secret + sizeof(client_secret), client_sig, client_sig_len);
  free(client_sig);
  char *client_secret_hex = hex_encode(client_pairing_secret, secret_len);
  free(client_pairing_secret);

  snprintf(path, sizeof(path), "/pair?devicename=roth&updateState=1&clientpairingsecret=%s&uniqueid=%s&uuid=%s",
           client_secret_hex, uid, random_hex(16));
  free(client_secret_hex);
  http_response r4 = http_get(http, path);
  if (r4.status != 200 || !paired_ok(r4.body)) {
    bench_log("фаза 4 не удалась: HTTP %ld", r4.status);
    http_response_free(&r4);
    goto out;
  }
  http_response_free(&r4);

  /* Фаза 5: подтверждение по HTTPS с клиентским сертификатом */
  snprintf(path, sizeof(path), "/pair?devicename=roth&updateState=1&phrase=pairchallenge&uniqueid=%s&uuid=%s", uid,
           random_hex(16));
  http_response r5 = http_get(https, path);
  if (r5.status != 200 || !paired_ok(r5.body)) {
    bench_log("фаза 5 не удалась: HTTP %ld", r5.status);
    http_response_free(&r5);
    goto out;
  }
  http_response_free(&r5);

  bench_log("пейринг выполнен (appversion %s)", appversion);
  rc = 0;

out:
  free(salt_hex);
  free(cert_hex);
  return rc;
}
