/*
 * Минимальный Wayland-клиент для стенда.
 *
 * wolf снимает кадры виртуального дисплея элементом waylanddisplaysrc —
 * это композитор. Пока к нему не подключится ни один клиент, кадров нет,
 * а без кадров нечего кодировать и нечего терять.
 *
 * Клиент поднимает xdg-shell-поверхность, рисует в wl_shm-буфер и
 * коммитит кадры по frame-callback'у композитора. Рисуем с движущимся
 * градиентом: соседние кадры различаются, поэтому по потоку видно, что
 * картинка живая.
 *
 * Протокол повторяет клиент из тестов wolf
 * (tests/platforms/linux/wayland-client.hpp): композитору нужен
 * xdg_shell-поверхность, голой wl_surface он не рисует.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wayland-client-protocol.h" /* генерируется wayland-scanner'ом */
#include "xdg-shell-client-protocol.h"

#define BUFFERS 2

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct wl_callback *frame_cb;
static struct wl_shm_pool *pool;
static struct wl_buffer *buffers[BUFFERS];
static uint32_t *pixels;
static uint32_t width = 640;
static uint32_t height = 480;
static uint32_t stride;
static uint32_t pool_size;
static int buffer_free[BUFFERS];
static int compositor_version = 1;
static uint32_t frame_no;
static int configured;
static int noise_mode;

static void handle_shm_format(void *data, struct wl_shm *s, uint32_t format) {
  (void)data;
  (void)s;
  (void)format;
}

static const struct wl_shm_listener shm_listener = {handle_shm_format};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                          uint32_t version) {
  (void)data;
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor_version = version < 4 ? (int)version : 4;
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, (uint32_t)compositor_version);
  } else if (strcmp(interface, "wl_shm") == 0) {
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    wl_shm_add_listener(shm, &shm_listener, NULL);
  } else if (strcmp(interface, "xdg_wm_base") == 0) {
    xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
  }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {handle_global, handle_global_remove};

static void handle_buffer_release(void *data, struct wl_buffer *buffer) {
  (void)data;
  for (int i = 0; i < BUFFERS; i++) {
    if (buffers[i] == buffer) {
      buffer_free[i] = 1;
    }
  }
}

static const struct wl_buffer_listener buffer_listener = {handle_buffer_release};

static void draw(void) {
  if (noise_mode) {
    /* Шум: кадры не сжимаются, кодировщик упирается в CPU и битрейт.
       Это эмуляция тяжёлой 3D-сцены (игры) — на статичной картинке
       кодировщик простаивает и конвейер никогда не отстаёт. */
    uint32_t state = 0x9e3779b9u ^ (frame_no * 2654435761u);
    for (uint32_t y = 0; y < height; y++) {
      uint32_t *row = pixels + (y * stride) / 4;
      for (uint32_t x = 0; x < width; x++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        row[x] = state;
      }
    }
    return;
  }
  for (uint32_t y = 0; y < height; y++) {
    uint32_t *row = pixels + (y * stride) / 4;
    for (uint32_t x = 0; x < width; x++) {
      uint32_t r = (x + frame_no * 4) & 0xff;
      uint32_t g = (y + frame_no * 2) & 0xff;
      uint32_t b = (frame_no * 8) & 0xff;
      row[x] = (r << 16) | (g << 8) | b; /* XRGB8888 */
    }
  }
}

static void commit_frame(void);

static void handle_frame(void *data, struct wl_callback *callback, uint32_t time) {
  (void)data;
  (void)time;
  wl_callback_destroy(callback);
  commit_frame();
}

static const struct wl_callback_listener frame_listener = {handle_frame};

static void handle_xdg_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {handle_xdg_ping};

static void handle_xdg_surface_configure(void *data, struct xdg_surface *xsurface, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(xsurface, serial);
  configured = 1;
  commit_frame();
}

static const struct xdg_surface_listener xdg_surface_listener = {handle_xdg_surface_configure};

static void handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                                           struct wl_array *states) {
  (void)data;
  (void)toplevel;
  (void)width;
  (void)height;
  (void)states;
}

static void handle_xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  (void)data;
  (void)toplevel;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {handle_xdg_toplevel_configure,
                                                                    handle_xdg_toplevel_close};

static void commit_frame(void) {
  int slot = -1;
  for (int i = 0; i < BUFFERS; i++) {
    if (buffer_free[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
  }
  buffer_free[slot] = 0;

  frame_no++;
  draw();

  wl_surface_attach(surface, buffers[slot], 0, 0);
  if (compositor_version >= 4) {
    wl_surface_damage_buffer(surface, 0, 0, (int32_t)width, (int32_t)height);
  } else {
    wl_surface_damage(surface, 0, 0, (int32_t)width, (int32_t)height);
  }
  frame_cb = wl_surface_frame(surface);
  wl_callback_add_listener(frame_cb, &frame_listener, NULL);
  wl_surface_commit(surface);

  if (getenv("WL_SHM_DEBUG")) {
    fprintf(stderr, "wl-shm-client: кадр %u\n", frame_no);
  }
}

int main(int argc, char **argv) {
  if (argc > 2) {
    width = (uint32_t)strtoul(argv[1], NULL, 10);
    height = (uint32_t)strtoul(argv[2], NULL, 10);
  }
  noise_mode = getenv("WL_SHM_NOISE") != NULL;

  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "wl-shm-client: не удалось подключиться к WAYLAND_DISPLAY=%s\n",
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(пусто)");
    return 1;
  }

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);

  if (!compositor || !shm || !xdg_wm_base) {
    fprintf(stderr, "wl-shm-client: композитор не дал wl_compositor/wl_shm/xdg_wm_base\n");
    return 1;
  }
  xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);

  if (getenv("WL_SHM_DEBUG")) {
    fprintf(stderr, "wl-shm-client: подключился, %ux%u, wl_compositor v%d\n", width, height, compositor_version);
  }

  stride = width * 4;
  pool_size = stride * height * BUFFERS;

  int fd = memfd_create("wl-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, pool_size) < 0) {
    fprintf(stderr, "wl-shm-client: не удалось создать shm-пул\n");
    return 1;
  }
  pixels = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    fprintf(stderr, "wl-shm-client: mmap не удался\n");
    return 1;
  }

  pool = wl_shm_create_pool(shm, fd, (int32_t)pool_size);
  for (int i = 0; i < BUFFERS; i++) {
    buffers[i] = wl_shm_pool_create_buffer(pool, (int32_t)(stride * height * i), (int32_t)width, (int32_t)height,
                                           (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffers[i], &buffer_listener, NULL);
    buffer_free[i] = 1;
  }

  surface = wl_compositor_create_surface(compositor);
  xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
  xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
  xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
  xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
  xdg_toplevel_set_title(xdg_toplevel, "wolf-bench");
  xdg_toplevel_set_app_id(xdg_toplevel, "wolf-bench");

  /* первый коммит без буфера — композитор ответит configure'ом */
  wl_surface_commit(surface);

  while (wl_display_dispatch(display) != -1) {
    /* обработка событий композитора */
  }
  (void)configured;
  return 0;
}
