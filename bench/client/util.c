#define _GNU_SOURCE
#include "bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bench_log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
  fflush(stderr);
}

char *hex_encode(const unsigned char *data, size_t len) {
  static const char digits[] = "0123456789abcdef";
  char *out = malloc(len * 2 + 1);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
  return out;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

int hex_decode(const char *hex, unsigned char *out, size_t out_len) {
  size_t len = strlen(hex);
  if (len != out_len * 2) {
    return -1;
  }
  for (size_t i = 0; i < out_len; i++) {
    int hi = hex_value(hex[i * 2]);
    int lo = hex_value(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return -1;
    }
    out[i] = (unsigned char)((hi << 4) | lo);
  }
  return 0;
}

void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
  unsigned int out_len = 0;
  EVP_Digest(data, len, out, &out_len, EVP_sha256(), NULL);
}

void aes128_ecb_encrypt(const unsigned char key[16], const unsigned char *in, size_t len, unsigned char *out) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL);
  EVP_CIPHER_CTX_set_padding(ctx, 0);
  int written = 0;
  EVP_EncryptUpdate(ctx, out, &written, in, (int)len);
  EVP_CIPHER_CTX_free(ctx);
}

void aes128_ecb_decrypt(const unsigned char key[16], const unsigned char *in, size_t len, unsigned char *out) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL);
  EVP_CIPHER_CTX_set_padding(ctx, 0);
  int written = 0;
  EVP_DecryptUpdate(ctx, out, &written, in, (int)len);
  EVP_CIPHER_CTX_free(ctx);
}

static char *dup_range(const char *begin, const char *end) {
  size_t len = (size_t)(end - begin);
  char *out = malloc(len + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, begin, len);
  out[len] = '\0';
  return out;
}

char *xml_tag(const char *xml, const char *tag) {
  if (!xml || !tag) {
    return NULL;
  }
  char open[128];
  char close[128];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);

  const char *begin = strstr(xml, open);
  if (!begin) {
    /* пустой элемент вида <tag/> */
    snprintf(open, sizeof(open), "<%s/>", tag);
    begin = strstr(xml, open);
    return begin ? strdup("") : NULL;
  }
  begin += strlen(open);
  const char *end = strstr(begin, close);
  if (!end) {
    return NULL;
  }
  return dup_range(begin, end);
}

char *xml_attr(const char *xml, const char *attr) {
  if (!xml || !attr) {
    return NULL;
  }
  /* ищем атрибут только в открывающем теге root */
  const char *root = strstr(xml, "<root");
  if (!root) {
    root = xml;
  }
  char pattern[128];
  snprintf(pattern, sizeof(pattern), "%s=\"", attr);
  const char *begin = strstr(root, pattern);
  if (!begin) {
    return NULL;
  }
  begin += strlen(pattern);
  const char *end = strchr(begin, '"');
  if (!end) {
    return NULL;
  }
  return dup_range(begin, end);
}
