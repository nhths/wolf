#define _GNU_SOURCE
#include "bench.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
} buffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  buffer *buf = userdata;
  size_t total = size * nmemb;
  char *grown = realloc(buf->data, buf->len + total + 1);
  if (!grown) {
    return 0;
  }
  buf->data = grown;
  memcpy(buf->data + buf->len, ptr, total);
  buf->len += total;
  buf->data[buf->len] = '\0';
  return total;
}

/* SHA-256 DER-сертификата из PEM */
static int pem_fingerprint(const char *pem, unsigned char out[32]) {
  BIO *bio = BIO_new_mem_buf(pem, -1);
  if (!bio) {
    return -1;
  }
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!cert) {
    return -1;
  }
  int rc = X509_digest(cert, EVP_sha256(), out, NULL);
  X509_free(cert);
  return rc == 1 ? 0 : -1;
}

/* Сертификат, который сервер показал в TLS, должен совпасть с тем, что
   получен при пейринге. Иначе это не тот сервер. */
static int pin_matches_peer(CURL *curl, const char *pin_path) {
  FILE *f = fopen(pin_path, "r");
  if (!f) {
    bench_log("нет сохранённого сертификата сервера: %s", pin_path);
    return 0;
  }
  char pinned[8192];
  size_t n = fread(pinned, 1, sizeof(pinned) - 1, f);
  fclose(f);
  pinned[n] = '\0';

  struct curl_certinfo *info = NULL;
  if (curl_easy_getinfo(curl, CURLINFO_CERTINFO, &info) != CURLE_OK || !info) {
    bench_log("не удалось получить сертификат сервера из TLS");
    return 0;
  }

  const char *peer_pem = NULL;
#if LIBCURL_VERSION_NUM >= 0x075800 && LIBCURL_VERSION_NUM < 0x080000
  /* curl 7.88–7.x: список структур с типом и данными */
  for (int i = 0; info[i].type != CURLINFO_CERTINFO_END; i++) {
    if (info[i].type == CURLINFO_CERTINFO_CERT) {
      peer_pem = info[i].data;
      break;
    }
  }
#else
  /* curl < 7.88 и curl >= 8.0: для каждого сертификата — список строк
     "имя:значение", сам сертификат лежит в записи "Cert:" */
  for (int i = 0; i < info->num_of_certs && !peer_pem; i++) {
    for (struct curl_slist *entry = info->certinfo[i]; entry; entry = entry->next) {
      if (strncmp(entry->data, "Cert:", 5) == 0) {
        const char *pem = entry->data + 5;
        while (*pem == ' ' || *pem == '\t') {
          pem++;
        }
        peer_pem = pem;
        break;
      }
    }
  }
#endif
  if (!peer_pem) {
    bench_log("сервер не показал сертификат");
    return 0;
  }

  unsigned char want[32];
  unsigned char got[32];
  if (pem_fingerprint(pinned, want) != 0 || pem_fingerprint(peer_pem, got) != 0) {
    bench_log("не удалось посчитать отпечаток сертификата");
    return 0;
  }
  if (memcmp(want, got, sizeof(want)) != 0) {
    bench_log("сертификат сервера не совпал с пиннированным (подмена?)");
    return 0;
  }
  return 1;
}

static http_response do_request(http_target *t, const char *method, const char *path,
                                const char *content_type, const char *body) {
  http_response resp = {0, NULL};
  CURL *curl = curl_easy_init();
  if (!curl) {
    return resp;
  }

  /* URL может быть длинным: в фазе пейринга в него уходит hex клиентского
     сертификата (~2.5 КБ), поэтому собираем его в куче. */
  size_t url_len = strlen(path) + strlen(t->host) + 32;
  char *url = malloc(url_len);
  if (!url) {
    curl_easy_cleanup(curl);
    return resp;
  }
  snprintf(url, url_len, "%s://%s:%d%s", t->tls ? "https" : "http", t->host, t->port, path);

  buffer buf = {NULL, 0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)t->timeout_s);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  /* Сертификат wolf самоподписанный и не выдан ни одним CA: цепочка и имя
     хоста не проверяются (как у Moonlight), но сам сертификат сверяется с
     пиннированным — см. pin_matches_peer. */
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
  if (t->cert_path) {
    curl_easy_setopt(curl, CURLOPT_SSLCERT, t->cert_path);
    curl_easy_setopt(curl, CURLOPT_SSLKEY, t->key_path);
  }
  struct curl_slist *headers = NULL;
  if (strcmp(method, "POST") == 0) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    if (content_type) {
      headers = curl_slist_append(headers, content_type);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
  }

  CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    bench_log("HTTP %s %s: ошибка %s", method, path, curl_easy_strerror(code));
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);
    free(buf.data);
    return resp;
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  resp.body = buf.data ? buf.data : strdup("");

  if (t->tls && t->pin_cert_path && !pin_matches_peer(curl, t->pin_cert_path)) {
    bench_log("HTTPS %s: сертификат сервера не прошёл проверку пина", path);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);
    free(resp.body);
    resp.body = NULL;
    resp.status = 0;
    return resp;
  }

  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  free(url);
  return resp;
}

http_response http_get(http_target *t, const char *path) {
  return do_request(t, "GET", path, NULL, NULL);
}

http_response http_post(http_target *t, const char *path, const char *content_type, const char *body) {
  return do_request(t, "POST", path, content_type, body);
}

void http_response_free(http_response *r) {
  free(r->body);
  r->body = NULL;
  r->status = 0;
}
