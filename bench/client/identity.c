#define _GNU_SOURCE
#include "bench.h"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_pem(const char *path, X509 *cert, EVP_PKEY *pkey) {
  FILE *f = fopen(path, "w");
  if (!f) {
    return -1;
  }
  int rc = 0;
  if (cert) {
    rc |= PEM_write_X509(f, cert) != 1;
  }
  if (pkey) {
    rc |= PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL) != 1;
  }
  fclose(f);
  chmod(path, 0600);
  return rc ? -1 : 0;
}

static int generate_identity(const char *cert_path, const char *key_path, const char *cn) {
  EVP_PKEY *pkey = EVP_RSA_gen(2048);
  if (!pkey) {
    bench_log("не удалось сгенерировать RSA-ключ");
    return -1;
  }

  X509 *cert = X509_new();
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365 * 10);
  X509_set_pubkey(cert, pkey);

  X509_NAME *name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"wolf-bench", -1, -1, 0);
  X509_set_issuer_name(cert, name);

  if (!X509_sign(cert, pkey, EVP_sha256())) {
    bench_log("не удалось подписать сертификат");
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }

  int rc = 0;
  if (write_pem(cert_path, cert, NULL) != 0) {
    bench_log("не удалось записать %s", cert_path);
    rc = -1;
  }
  if (write_pem(key_path, NULL, pkey) != 0) {
    bench_log("не удалось записать %s", key_path);
    rc = -1;
  }

  X509_free(cert);
  EVP_PKEY_free(pkey);
  return rc;
}

int identity_load_or_create(client_identity *id, const char *dir, const char *cn) {
  memset(id, 0, sizeof(*id));

  char cert_path[512];
  char key_path[512];
  snprintf(cert_path, sizeof(cert_path), "%s/client.crt", dir);
  snprintf(key_path, sizeof(key_path), "%s/client.key", dir);

  mkdir(dir, 0700);

  int fresh = access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0;
  if (fresh && generate_identity(cert_path, key_path, cn) != 0) {
    return -1;
  }

  FILE *f = fopen(cert_path, "r");
  if (!f) {
    bench_log("не удалось открыть %s", cert_path);
    return -1;
  }
  id->cert = PEM_read_X509(f, NULL, NULL, NULL);
  fclose(f);
  if (!id->cert) {
    bench_log("не удалось разобрать сертификат %s", cert_path);
    return -1;
  }

  f = fopen(key_path, "r");
  if (!f) {
    bench_log("не удалось открыть %s", key_path);
    return -1;
  }
  id->pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
  fclose(f);
  if (!id->pkey) {
    bench_log("не удалось разобрать закрытый ключ %s", key_path);
    return -1;
  }

  const ASN1_BIT_STRING *sig = NULL;
  X509_get0_signature(&sig, NULL, id->cert);
  if (!sig) {
    bench_log("у сертификата нет подписи");
    return -1;
  }
  id->cert_signature_len = (size_t)ASN1_STRING_length(sig);
  id->cert_signature = malloc(id->cert_signature_len);
  memcpy(id->cert_signature, ASN1_STRING_get0_data(sig), id->cert_signature_len);

  id->cert_path = strdup(cert_path);
  id->key_path = strdup(key_path);
  id->dir = strdup(dir);
  if (fresh) {
    bench_log("создана новая идентичность клиента: %s", cert_path);
  }
  return 0;
}

void identity_free(client_identity *id) {
  if (id->cert) {
    X509_free(id->cert);
  }
  if (id->pkey) {
    EVP_PKEY_free(id->pkey);
  }
  free(id->cert_signature);
  free(id->cert_path);
  free(id->key_path);
  free(id->dir);
  memset(id, 0, sizeof(*id));
}
