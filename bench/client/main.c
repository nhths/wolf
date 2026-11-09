#define _GNU_SOURCE
#include "bench.h"

#include <Limelight.h>

#include <openssl/rand.h>

#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  const char *host;
  int http_port;
  int https_port;
  int width;
  int height;
  int fps;
  int bitrate;
  int packet_size;
  const char *app_id;
  const char *app_title;
  const char *pin;
  const char *identity_dir;
  int duration;
  int skip_pairing;
  int enable_rfi;
  int remote;
  int check;
  int no_audio;
} options;

typedef struct {
  unsigned long frames;
  unsigned long idr_frames;
  unsigned long lost_frames;
  unsigned long gaps;
  double max_assembly_ms;
  double total_assembly_ms;
  int last_frame_number;
  int poor_status_events;
  int terminated;
  int termination_code;
} metrics;

static metrics g_metrics;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "Использование: %s --host HOST [опции]\n"
          "\n"
          "  --host HOST          адрес wolf (по умолчанию 127.0.0.1)\n"
          "  --http-port PORT     порт HTTP/pair (по умолчанию 47989)\n"
          "  --https-port PORT    порт HTTPS API (по умолчанию 47984)\n"
          "  --width N            ширина потока (по умолчанию 1920)\n"
          "  --height N           высота потока (по умолчанию 1080)\n"
          "  --fps N              кадров в секунду (по умолчанию 60)\n"
          "  --bitrate N          битрейт в Кбит/с (по умолчанию 20000)\n"
          "  --packet-size N      размер RTP-пакета (по умолчанию 1024)\n"
          "  --app-id ID          id приложения для запуска\n"
          "  --app-title TEXT     выбрать приложение по названию\n"
          "  --pin PIN            PIN для пейринга (по умолчанию 1234)\n"
          "  --identity-dir DIR   каталог сертификата (по умолчанию ./identity)\n"
          "  --duration N         сколько секунд держать поток (0 — до разрыва)\n"
          "  --skip-pairing       не пейрить (сертификат уже в списке wolf)\n"
          "  --rfi                объявить поддержку reference frame invalidation\n"
          "  --remote             считать адрес удалённым (packet-size 1024)\n"
          "  --check              только пейринг и список приложений, без потока\n"
          "  --no-audio           не запрашивать звук (у wolf может не быть аудиосервера)\n",
          argv0);
}

/* --- колбэки moonlight --- */

static int on_decode_unit(PDECODE_UNIT unit) {
  if (!unit) {
    return 0;
  }
  g_metrics.frames++;
  if (unit->frameType == FRAME_TYPE_IDR) {
    g_metrics.idr_frames++;
  }
  if (g_metrics.last_frame_number >= 0 && unit->frameNumber > g_metrics.last_frame_number + 1) {
    g_metrics.gaps++;
    g_metrics.lost_frames += (unsigned long)(unit->frameNumber - g_metrics.last_frame_number - 1);
  }
  g_metrics.last_frame_number = unit->frameNumber;

  if (unit->enqueueTimeUs > unit->receiveTimeUs) {
    double assembly_ms = (double)(unit->enqueueTimeUs - unit->receiveTimeUs) / 1000.0;
    g_metrics.total_assembly_ms += assembly_ms;
    if (assembly_ms > g_metrics.max_assembly_ms) {
      g_metrics.max_assembly_ms = assembly_ms;
    }
  }
  return 0;
}

static int on_video_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  bench_log("декодер: формат=%d %dx%d@%d", videoFormat, width, height, redrawRate);
  return 0;
}

static void on_video_start(void) {}
static void on_video_stop(void) {}
static void on_video_cleanup(void) {}

static int on_audio_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void *context,
                         int arFlags) {
  return 0;
}

static void on_audio_start(void) {}
static void on_audio_stop(void) {}
static void on_audio_cleanup(void) {}
static void on_audio_sample(char *sampleData, int sampleLength) {}

/* Логи самого moonlight-common-c: там, в частности, «Network dropped N frames» */
static void on_log_message(const char *format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  bench_log("moonlight: %s", buf);
}

static void on_stage_starting(int stage) { bench_log("этап: %s", LiGetStageName(stage)); }
static void on_stage_complete(int stage) { bench_log("этап завершён: %s", LiGetStageName(stage)); }
static void on_stage_failed(int stage, int error) {
  bench_log("этап провален: %s (%d)", LiGetStageName(stage), error);
}
static void on_connection_started(void) { bench_log("поток запущен"); }
static void on_connection_terminated(int errorCode) {
  g_metrics.terminated = 1;
  g_metrics.termination_code = errorCode;
  bench_log("соединение прервано, код %d", errorCode);
}
static void on_connection_status(int status) {
  if (status == CONN_STATUS_POOR) {
    g_metrics.poor_status_events++;
    bench_log("статус соединения: POOR");
  } else {
    bench_log("статус соединения: %d", status);
  }
}

/* --- вспомогательное --- */

static char *find_app_id(const char *applist_xml, const char *title_filter) {
  const char *cursor = applist_xml;
  char *first = NULL;
  char *matched = NULL;
  while ((cursor = strstr(cursor, "<App>")) != NULL) {
    const char *end = strstr(cursor, "</App>");
    if (!end) {
      break;
    }
    size_t len = (size_t)(end - cursor);
    char *block = malloc(len + 1);
    memcpy(block, cursor, len);
    block[len] = '\0';

    char *id = xml_tag(block, "ID");
    char *title = xml_tag(block, "AppTitle");
    if (id && title) {
      if (first == NULL) {
        first = id;
        id = NULL;
      }
      if (!matched && title_filter && *title_filter && strcasestr(title, title_filter)) {
        matched = id;
        id = NULL;
      }
    }
    free(id);
    free(title);
    free(block);
    if (matched) {
      break;
    }
    cursor = end + 1;
  }
  /* фильтр по названию может не совпасть с тем, как приложение названо на
     этой машине — тогда берём первое доступное */
  if (matched) {
    free(first);
    return matched;
  }
  return first;
}

int main(int argc, char **argv) {
  options opt = {
      .host = "127.0.0.1",
      .http_port = 47989,
      .https_port = 47984,
      .width = 1920,
      .height = 1080,
      .fps = 60,
      .bitrate = 20000,
      .packet_size = 1024,
      .app_id = NULL,
      .app_title = NULL,
      .pin = "1234",
      .identity_dir = "./identity",
      .duration = 60,
      .skip_pairing = 0,
      .enable_rfi = 0,
      .remote = 0,
  };

  static struct option long_options[] = {
      {"host", required_argument, 0, 'h'},
      {"http-port", required_argument, 0, 'P'},
      {"https-port", required_argument, 0, 'S'},
      {"width", required_argument, 0, 'w'},
      {"height", required_argument, 0, 'H'},
      {"fps", required_argument, 0, 'f'},
      {"bitrate", required_argument, 0, 'b'},
      {"packet-size", required_argument, 0, 'k'},
      {"app-id", required_argument, 0, 'a'},
      {"app-title", required_argument, 0, 't'},
      {"pin", required_argument, 0, 'p'},
      {"identity-dir", required_argument, 0, 'i'},
      {"duration", required_argument, 0, 'd'},
      {"skip-pairing", no_argument, 0, 's'},
      {"rfi", no_argument, 0, 'r'},
      {"remote", no_argument, 0, 'R'},
      {"check", no_argument, 0, 'c'},
      {"no-audio", no_argument, 0, 'N'},
      {"help", no_argument, 0, '?'},
      {0, 0, 0, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
    switch (c) {
      case 'h': opt.host = optarg; break;
      case 'P': opt.http_port = atoi(optarg); break;
      case 'S': opt.https_port = atoi(optarg); break;
      case 'w': opt.width = atoi(optarg); break;
      case 'H': opt.height = atoi(optarg); break;
      case 'f': opt.fps = atoi(optarg); break;
      case 'b': opt.bitrate = atoi(optarg); break;
      case 'k': opt.packet_size = atoi(optarg); break;
      case 'a': opt.app_id = optarg; break;
      case 't': opt.app_title = optarg; break;
      case 'p': opt.pin = optarg; break;
      case 'i': opt.identity_dir = optarg; break;
      case 'd': opt.duration = atoi(optarg); break;
      case 's': opt.skip_pairing = 1; break;
      case 'r': opt.enable_rfi = 1; break;
      case 'R': opt.remote = 1; break;
      case 'c': opt.check = 1; break;
      case 'N': opt.no_audio = 1; break;
      default: usage(argv[0]); return 2;
    }
  }

  setvbuf(stdout, NULL, _IOLBF, 0);

  client_identity identity;
  if (identity_load_or_create(&identity, opt.identity_dir, "wolf-bench") != 0) {
    return 1;
  }

  /* wolf не отдаёт ответ на первую фазу пейринга, пока не введён PIN
     (pair_phase1 ждёт промис), поэтому таймаут должен быть с запасом. */
  char server_pin_path[512];
  snprintf(server_pin_path, sizeof(server_pin_path), "%s/server.pem", opt.identity_dir);
  http_target http = {opt.host, opt.http_port, 0, NULL, NULL, NULL, 180};
  http_target https = {opt.host, opt.https_port, 1, identity.cert_path, identity.key_path, server_pin_path, 180};

  /* 1. serverinfo по обычному HTTP — доступен без пейринга */
  http_response si = http_get(&http, "/serverinfo");
  if (si.status != 200) {
    bench_log("не удалось получить serverinfo: HTTP %ld", si.status);
    http_response_free(&si);
    return 1;
  }
  char *appversion = xml_tag(si.body, "appversion");
  char *gfe_version = xml_tag(si.body, "GfeVersion");
  char *codec_support_str = xml_tag(si.body, "ServerCodecModeSupport");
  if (!appversion) {
    bench_log("в serverinfo нет appversion");
    free(gfe_version);
    free(codec_support_str);
    http_response_free(&si);
    return 1;
  }
  int codec_support = codec_support_str ? atoi(codec_support_str) : SCM_H264;
  bench_log("serverinfo: appversion=%s codec_support=0x%x", appversion, codec_support);
  free(codec_support_str);
  http_response_free(&si);

  /* 2. пейринг */
  if (!opt.skip_pairing) {
    char *server_pem = NULL;
    if (pair_host(&http, &https, &identity, opt.pin, appversion, &server_pem) != 0) {
      bench_log("пейринг не удался");
      free(appversion);
      free(gfe_version);
      identity_free(&identity);
      return 1;
    }
    free(server_pem);
  }

  /* 3. serverinfo по HTTPS — уже с клиентским сертификатом */
  si = http_get(&https, "/serverinfo");
  if (si.status != 200) {
    bench_log("HTTPS serverinfo не прошёл: HTTP %ld (клиент не пейрен?)", si.status);
    http_response_free(&si);
    free(appversion);
    free(gfe_version);
    identity_free(&identity);
    return 1;
  }
  http_response_free(&si);

  /* 4. список приложений */
  char *app_id = NULL;
  if (!opt.app_id) {
    http_response al = http_get(&https, "/applist");
    if (al.status == 200) {
      app_id = find_app_id(al.body, opt.app_title);
    } else {
      bench_log("applist не отдал список: HTTP %ld", al.status);
    }
    if (opt.check) {
      printf("{\"applist_ok\": %s}\n", al.status == 200 ? "true" : "false");
    }
    http_response_free(&al);
  } else {
    app_id = strdup(opt.app_id);
  }
  if (!app_id) {
    bench_log("не выбрано приложение для запуска (--app-id или --app-title)");
    free(appversion);
    free(gfe_version);
    identity_free(&identity);
    return 1;
  }
  bench_log("приложение: %s", app_id);

  if (opt.check) {
    bench_log("режим --check: пейринг и список приложений проверены");
    free(app_id);
    free(appversion);
    free(gfe_version);
    identity_free(&identity);
    return 0;
  }

  /* 5. ключи шифрования ввода. Формат — как у настоящего Moonlight:
        rikey = hex 16 байт, rikeyid = десятичное число из первых 4 байт IV. */
  unsigned char rikey[16];
  unsigned char rikeyid_raw[4];
  RAND_bytes(rikey, sizeof(rikey));
  RAND_bytes(rikeyid_raw, sizeof(rikeyid_raw));
  int32_t rikeyid = (int32_t)(((uint32_t)rikeyid_raw[0] << 24) | ((uint32_t)rikeyid_raw[1] << 16) |
                               ((uint32_t)rikeyid_raw[2] << 8) | (uint32_t)rikeyid_raw[3]);
  char *rikey_hex = hex_encode(rikey, sizeof(rikey));

  char launch_path[1024];
  snprintf(launch_path, sizeof(launch_path),
           "/launch?appid=%s&mode=%dx%dx%d&additionalStates=1&sops=0&rikey=%s&rikeyid=%d"
           "&localAudioPlayMode=0&surroundAudioInfo=196610&remoteControllersBitmap=0&gcmap=0&gcpersist=0&corever=1",
           app_id, opt.width, opt.height, opt.fps, rikey_hex, rikeyid);
  free(rikey_hex);

  http_response launch = http_get(&https, launch_path);
  if (launch.status != 200) {
    bench_log("launch не прошёл: HTTP %ld", launch.status);
    http_response_free(&launch);
    free(app_id);
    free(appversion);
    free(gfe_version);
    identity_free(&identity);
    return 1;
  }
  char *session_url = xml_tag(launch.body, "sessionUrl0");
  http_response_free(&launch);
  if (!session_url) {
    bench_log("в ответе launch нет sessionUrl0");
    free(app_id);
    free(appversion);
    free(gfe_version);
    identity_free(&identity);
    return 1;
  }
  bench_log("session url: %s", session_url);

  /* 6. стрим */
  STREAM_CONFIGURATION stream_config;
  LiInitializeStreamConfiguration(&stream_config);
  stream_config.width = opt.width;
  stream_config.height = opt.height;
  stream_config.fps = opt.fps;
  stream_config.bitrate = opt.bitrate;
  stream_config.packetSize = opt.packet_size;
  stream_config.streamingRemotely = opt.remote ? STREAM_CFG_REMOTE : STREAM_CFG_AUTO;
  stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  stream_config.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
  stream_config.clientRefreshRateX100 = opt.fps * 100;
  stream_config.colorSpace = COLORSPACE_REC_709;
  stream_config.colorRange = COLOR_RANGE_LIMITED;
  memcpy(stream_config.remoteInputAesKey, rikey, sizeof(rikey));
  memset(stream_config.remoteInputAesIv, 0, sizeof(stream_config.remoteInputAesIv));
  memcpy(stream_config.remoteInputAesIv, rikeyid_raw, sizeof(rikeyid_raw));

  DECODER_RENDERER_CALLBACKS video_callbacks;
  LiInitializeVideoCallbacks(&video_callbacks);
  video_callbacks.setup = on_video_setup;
  video_callbacks.start = on_video_start;
  video_callbacks.stop = on_video_stop;
  video_callbacks.cleanup = on_video_cleanup;
  video_callbacks.submitDecodeUnit = on_decode_unit;
  if (opt.enable_rfi) {
    video_callbacks.capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC |
                                   CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
  }

  AUDIO_RENDERER_CALLBACKS audio_callbacks;
  LiInitializeAudioCallbacks(&audio_callbacks);
  audio_callbacks.init = on_audio_init;
  audio_callbacks.start = on_audio_start;
  audio_callbacks.stop = on_audio_stop;
  audio_callbacks.cleanup = on_audio_cleanup;
  audio_callbacks.decodeAndPlaySample = on_audio_sample;

  CONNECTION_LISTENER_CALLBACKS listener;
  LiInitializeConnectionCallbacks(&listener);
  listener.logMessage = on_log_message;
  listener.stageStarting = on_stage_starting;
  listener.stageComplete = on_stage_complete;
  listener.stageFailed = on_stage_failed;
  listener.connectionStarted = on_connection_started;
  listener.connectionTerminated = on_connection_terminated;
  listener.connectionStatusUpdate = on_connection_status;

  SERVER_INFORMATION server_info;
  LiInitializeServerInformation(&server_info);
  server_info.address = opt.host;
  server_info.serverInfoAppVersion = appversion;
  server_info.serverInfoGfeVersion = gfe_version;
  server_info.rtspSessionUrl = session_url;
  server_info.serverCodecModeSupport = codec_support;

  g_metrics.last_frame_number = -1;
  double started = now_ms();
  int rc = LiStartConnection(&server_info, &stream_config, &listener, &video_callbacks, &audio_callbacks, NULL, 0, NULL,
                             0);
  if (rc != 0) {
    bench_log("LiStartConnection вернул %d", rc);
    free(app_id);
    free(appversion);
    free(gfe_version);
    free(session_url);
    identity_free(&identity);
    return 1;
  }

  while (!g_metrics.terminated) {
    if (opt.duration > 0 && now_ms() - started > (double)opt.duration * 1000.0) {
      break;
    }
    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }

  LiStopConnection();

  double elapsed = (now_ms() - started) / 1000.0;
  printf("{\"frames\": %lu, \"idr_frames\": %lu, \"gaps\": %lu, \"lost_frames\": %lu, "
         "\"poor_status\": %d, \"terminated\": %d, \"termination_code\": %d, \"elapsed_s\": %.1f, "
         "\"avg_assembly_ms\": %.2f, \"max_assembly_ms\": %.2f}\n",
         g_metrics.frames, g_metrics.idr_frames, g_metrics.gaps, g_metrics.lost_frames, g_metrics.poor_status_events,
         g_metrics.terminated, g_metrics.termination_code, elapsed,
         g_metrics.frames ? g_metrics.total_assembly_ms / (double)g_metrics.frames : 0.0, g_metrics.max_assembly_ms);

  free(app_id);
  free(appversion);
  free(gfe_version);
  free(session_url);
  identity_free(&identity);
  return g_metrics.terminated ? 3 : 0;
}
