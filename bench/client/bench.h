#pragma once

#include <stddef.h>

#include <openssl/evp.h>
#include <openssl/x509.h>

/* Шестнадцатеричные и бинарные помощники */
char *hex_encode(const unsigned char *data, size_t len);
int hex_decode(const char *hex, unsigned char *out, size_t out_len);

/* SHA-256, AES-128-ECB без паддинга */
void sha256(const unsigned char *data, size_t len, unsigned char out[32]);
void aes128_ecb_encrypt(const unsigned char key[16], const unsigned char *in, size_t len, unsigned char *out);
void aes128_ecb_decrypt(const unsigned char key[16], const unsigned char *in, size_t len, unsigned char *out);

/* Разбор ответов wolf: <tag>значение</tag> и атрибут корневого тега */
char *xml_tag(const char *xml, const char *tag);
char *xml_attr(const char *xml, const char *attr);

/* HTTP(S) через libcurl. cert_path == NULL — без клиентского сертификата.
   pin_cert_path — сертификат сервера, полученный при пейринге: показанный
   сервером сертификат сверяется с ним, как это делает настоящий Moonlight
   (moonlight-qt: ошибки SSL игнорируются только для пиннированного cert). */
typedef struct {
  const char *host;
  int port;
  int tls; /* 1 — https */
  const char *cert_path;
  const char *key_path;
  const char *pin_cert_path;
  int timeout_s;
} http_target;

typedef struct {
  long status;
  char *body;
} http_response;

http_response http_get(http_target *t, const char *path);
http_response http_post(http_target *t, const char *path, const char *content_type, const char *body);
void http_response_free(http_response *r);

/* Идентичность клиента: X.509-сертификат и закрытый ключ */
typedef struct {
  X509 *cert;
  EVP_PKEY *pkey;
  char *dir;
  char *cert_path;
  char *key_path;
  unsigned char *cert_signature;
  size_t cert_signature_len;
} client_identity;

int identity_load_or_create(client_identity *id, const char *dir, const char *cn);
void identity_free(client_identity *id);

/* Пейринг Moonlight: фазы 1-4 по HTTP, фаза 5 по HTTPS */
int pair_host(http_target *http, http_target *https, client_identity *id,
              const char *pin, const char *appversion, char **server_cert_pem);

/* Лог клиента — в stderr, чтобы stdout остался чистым для метрик */
void bench_log(const char *fmt, ...);
