#define _GNU_SOURCE
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo.h>
#include <leptonica/allheaders.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <tesseract/capi.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
extern char **environ;

struct buffer {
  struct wl_buffer *wl;
  cairo_surface_t *cairo;
  int busy; /* 1 while the compositor is using it */
};

struct box {
  double x, y, w, h;
};

/* globals from the registry */
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_display *display;

/* surface and its buffers */
static struct wl_surface *surface;
static int width, height;
static struct buffer buffers[2];
static unsigned char *pool_data;
static size_t pool_size;
static struct wl_callback *frame_cb; /* non-NULL while waiting for a frame */
static int needs_redraw;

/* cursor */
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor_image *cursor_image;
static struct wl_surface *cursor_surface;

/* selection state */
static double ptr_x, ptr_y; /* current pointer position */
static int dragging;
static double drag_x, drag_y; /* where the drag started */
static struct box theBox;

static int running = 1;
static int capture_pending;
static int reading_pending;

/* turn two corners into a box with positive width/height */
static struct box make_box(double x0, double y0, double x1, double y1) {
  struct box b;
  b.x = x0 < x1 ? x0 : x1;
  b.y = y0 < y1 ? y0 : y1;
  b.w = x0 < x1 ? x1 - x0 : x0 - x1;
  b.h = y0 < y1 ? y1 - y0 : y0 - y1;
  return b;
}

/* ---- OCR ---- */
#define OCR_PATH "/tmp/overlay-ocr.png"

enum mode { MODE_SHOT, MODE_TEXT };
static enum mode mode = MODE_SHOT;

struct word {
  char *text;
  struct box r;
  int line;
  int selected;
};
static struct word *words;
static int nwords;

static int sel_dragging;
static double sel_x, sel_y;
static int configured; /* 0 while hidden, until a new configure arrives */

static int inside(struct box b, double x, double y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}
static int overlaps(struct box a, struct box b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
         b.y < a.y + a.h;
}
static void free_words(void) {
  for (int i = 0; i < nwords; i++)
    free(words[i].text);
  free(words);
  words = NULL;
  nwords = 0;
}

static void run_ocr(const char *path, struct box region) {
  free_words();
  PIX *pix = pixRead(path);
  if (!pix)
    return;
  /* image pixels per logical pixel (2 on a 2x HiDPI screen) */
  double scale = pixGetWidth(pix) / region.w;

  TessBaseAPI *api = TessBaseAPICreate();
  if (TessBaseAPIInit3(api, NULL, "eng") == 0) {
    TessBaseAPISetImage2(api, pix);
    if (TessBaseAPIRecognize(api, NULL) == 0) {
      TessResultIterator *ri = TessBaseAPIGetIterator(api);
      if (ri) {
        TessPageIterator *pi = TessResultIteratorGetPageIterator(ri);
        int line = -1, cap = 0;
        do {
          if (TessPageIteratorIsAtBeginningOf(pi, RIL_TEXTLINE))
            line++;
          char *txt = TessResultIteratorGetUTF8Text(ri, RIL_WORD);
          int x1, y1, x2, y2;
          if (txt && *txt &&
              TessPageIteratorBoundingBox(pi, RIL_WORD, &x1, &y1, &x2, &y2)) {
            if (nwords == cap) {
              cap = cap ? cap * 2 : 64;
              words = realloc(words, cap * sizeof *words);
            }
            words[nwords++] = (struct word){
                .text = strdup(txt),
                .r = {region.x + x1 / scale, region.y + y1 / scale,
                      (x2 - x1) / scale, (y2 - y1) / scale},
                .line = line,
            };
          }
          if (txt)
            TessDeleteText(txt);
        } while (TessPageIteratorNext(pi, RIL_WORD));
        TessResultIteratorDelete(ri);
      }
    }
  } else {
    fprintf(stderr,
            "tesseract init failed (is tesseract-data-eng installed?)\n");
  }
  TessBaseAPIDelete(api);
  pixDestroy(&pix);
}

static void hide_overlay(void) {
  if (frame_cb) {
    wl_callback_destroy(frame_cb);
    frame_cb = NULL;
  }
  configured = 0;
  wl_surface_attach(surface, NULL, 0, 0);
  wl_surface_commit(surface);
  wl_display_roundtrip(display);
}

static void show_overlay(void) {
  wl_surface_commit(surface); /* compositor replies with configure → redraw */
}

static void run_grim(struct box b, const char *path) {
  char geom[64];
  snprintf(geom, sizeof geom, "%d,%d %dx%d", (int)b.x, (int)b.y, (int)b.w,
           (int)b.h);
  char *argv[] = {"grim", "-g", geom, (char *)path, NULL};
  pid_t pid;
  if (posix_spawnp(&pid, "grim", NULL, NULL, argv, environ) == 0)
    waitpid(pid, NULL, 0);
  else
    perror("grim");
}

static void update_selection(void) {
  struct box s = make_box(sel_x, sel_y, ptr_x, ptr_y);
  if (s.w < 1)
    s.w = 1; /* a plain click still selects one word */
  if (s.h < 1)
    s.h = 1;
  for (int i = 0; i < nwords; i++)
    words[i].selected = overlaps(s, words[i].r);
}

static void copy_selection(void) {
  int any = 0;
  for (int i = 0; i < nwords; i++)
    any |= words[i].selected;
  if (!any)
    return;

  FILE *p = popen("wl-copy", "w");
  if (!p)
    return;
  int first = 1, prev_line = -1;
  for (int i = 0; i < nwords; i++) {
    if (!words[i].selected)
      continue;
    if (!first)
      fputc(words[i].line != prev_line ? '\n' : ' ', p);
    fputs(words[i].text, p);
    prev_line = words[i].line;
    first = 0;
  }
  pclose(p);
}

static void draw_words(cairo_t *cr) {
  for (int i = 0; i < nwords; i++) {
    struct box r = words[i].r;

    /* backing: blue if selected, dark otherwise */
    if (words[i].selected)
      cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.85);
    else
      cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);

    /* text sized to the word's box */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(font, r.h * 0.8 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, words[i].text, -1);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_save(cr);
    cairo_translate(cr, r.x, r.y + (r.h - th) / 2.0);
    if (tw > 0)
      cairo_scale(cr, r.w / tw, 1.0);      /* stretch to the box width */
    pango_cairo_update_layout(cr, layout); /* required after transforming */
    cairo_set_source_rgb(cr, 1, 1, 1);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(font);
    g_object_unref(layout);
  }
}

/* ---- OCR END ---- */

static void capture_box(struct box b) {
  /* hide: a NULL buffer unmaps the surface */
  if (frame_cb) {
    wl_callback_destroy(frame_cb);
    frame_cb = NULL;
  }
  wl_surface_attach(surface, NULL, 0, 0);
  wl_surface_commit(surface);
  wl_display_roundtrip(display);

  /* run grim */
  time_t now = time(NULL);
  char name[64];
  strftime(name, sizeof name, "%Y-%m-%d_%H%M%S-ocra.png", localtime(&now));
  char path[512];
  snprintf(path, sizeof path, "%s/Pictures/%s", getenv("HOME"), name);

  char geom[64];
  snprintf(geom, sizeof geom, "%d,%d %dx%d", (int)b.x, (int)b.y, (int)b.w,
           (int)b.h);
  char *argv[] = {"grim", "-g", geom, path, NULL};
  pid_t pid;
  if (posix_spawnp(&pid, "grim", NULL, NULL, argv, environ) == 0)
    waitpid(pid, NULL, 0);
  else
    perror("grim");

  /* show again: an empty commit triggers a new configure, then a redraw */
  wl_surface_commit(surface);
}

/* ---------- buffers (created once, reused) ---------- */
static void render(void);

static void buffer_release(void *data, struct wl_buffer *wl_buf) {
  struct buffer *b = data;
  b->busy = 0;
  if (needs_redraw && !frame_cb)
    render(); /* we were waiting for a free buffer */
}
static const struct wl_buffer_listener buffer_listener = {.release =
                                                              buffer_release};

static void destroy_buffers(void) {
  for (int i = 0; i < 2; i++) {
    if (buffers[i].cairo)
      cairo_surface_destroy(buffers[i].cairo);
    if (buffers[i].wl)
      wl_buffer_destroy(buffers[i].wl);
    buffers[i] = (struct buffer){0};
  }
  if (pool_data) {
    munmap(pool_data, pool_size);
    pool_data = NULL;
  }
}

static int create_buffers(void) {
  int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  size_t size = (size_t)stride * height;
  pool_size = size * 2; /* room for two buffers */

  int fd = memfd_create("overlay", MFD_CLOEXEC);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, pool_size) < 0) {
    close(fd);
    return -1;
  }
  pool_data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pool_data == MAP_FAILED) {
    pool_data = NULL;
    close(fd);
    return -1;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)pool_size);
  for (int i = 0; i < 2; i++) {
    buffers[i].wl =
        wl_shm_pool_create_buffer(pool, (int32_t)(i * size), width, height,
                                  stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buffers[i].wl, &buffer_listener, &buffers[i]);
    buffers[i].cairo = cairo_image_surface_create_for_data(
        pool_data + i * size, CAIRO_FORMAT_ARGB32, width, height, stride);
    buffers[i].busy = 0;
  }
  wl_shm_pool_destroy(pool);
  close(fd); /* we keep pool_data mapped so we can redraw */
  return 0;
}

/* ---------- drawing ---------- */
static void draw_label(cairo_t *cr, const char *text, double x, double y) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font =
      pango_font_description_from_string("Sans Bold 14");
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

static void paint(struct buffer *b) {
  cairo_t *cr = cairo_create(b->cairo);

  /* dim background */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
  cairo_paint(cr);

  struct box bx = dragging ? make_box(drag_x, drag_y, ptr_x, ptr_y) : theBox;

  /* finished theBox, plus the one being dragged */

  /* clear the inside so the desktop shows through, like slurp */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_rectangle(cr, bx.x, bx.y, bx.w, bx.h);
  cairo_fill(cr);

  /* outline */
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
  cairo_set_line_width(cr, 2);
  cairo_rectangle(cr, bx.x, bx.y, bx.w, bx.h);
  cairo_stroke(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgb(cr, 1, 1, 1);

  if (mode == MODE_TEXT)
    draw_words(cr);
  if (dragging) {
    char label[32];
    snprintf(label, sizeof label, "%d × %d", (int)bx.w, (int)bx.h);
    draw_label(cr, label, bx.x, bx.y + bx.h + 6);
  } else {
    if (mode == MODE_TEXT)
      draw_label(cr, "OCR", 20, 20);
    else if (mode == MODE_SHOT)
      draw_label(cr, "Screenshot", 20, 20);
  }

  cairo_destroy(cr);
  cairo_surface_flush(b->cairo);
}

/* ---------- frame pacing ---------- */
static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  wl_callback_destroy(cb);
  frame_cb = NULL;
  if (needs_redraw)
    render(); /* something changed since the last frame */
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void render(void) {
  if (!configured)
    return;
  struct buffer *b = NULL;
  for (int i = 0; i < 2; i++)
    if (buffers[i].wl && !buffers[i].busy) {
      b = &buffers[i];
      break;
    }
  if (!b)
    return; /* both in use; buffer_release will retry */

  needs_redraw = 0;
  paint(b);

  frame_cb = wl_surface_frame(surface); /* "tell me when to draw next" */
  wl_callback_add_listener(frame_cb, &frame_listener, NULL);
  wl_surface_attach(surface, b->wl, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, width, height);
  wl_surface_commit(surface);
  b->busy = 1;
}

/* mark that the screen is out of date; draw now if allowed, else at next frame
 */
static void request_redraw(void) {
  needs_redraw = 1;
  if (!frame_cb)
    render();
}

/* ---------- layer surface ---------- */
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  configured = 1;
  if ((int)w != width || (int)h != height || !pool_data) {
    destroy_buffers();
    width = w;
    height = h;
    if (create_buffers() < 0) {
      running = 0;
      return;
    }
  }
  request_redraw();
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
  running = 0;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* ---------- pointer ---------- */
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
  ptr_x = wl_fixed_to_double(sx);
  ptr_y = wl_fixed_to_double(sy);
  if (cursor_image)
    wl_pointer_set_cursor(p, serial, cursor_surface, cursor_image->hotspot_x,
                          cursor_image->hotspot_y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy) {
  ptr_x = wl_fixed_to_double(sx);
  ptr_y = wl_fixed_to_double(sy);
  if (sel_dragging)
    update_selection();
  if (dragging || sel_dragging)
    request_redraw();
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
  if (button != BTN_LEFT)
    return;
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (mode == MODE_TEXT && inside(theBox, ptr_x, ptr_y)) {
      sel_dragging = 1; /* selecting text */
      sel_x = ptr_x;
      sel_y = ptr_y;
      update_selection();
    } else {
      free_words();
      dragging = 1;
      drag_x = ptr_x;
      drag_y = ptr_y;
    }
  } else if (sel_dragging) {
    sel_dragging = 0;
    copy_selection();
  } else if (dragging) {
    dragging = 0;
    theBox = make_box(drag_x, drag_y, ptr_x, ptr_y);
  }
  request_redraw();
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
};

/* ---------- keyboard ---------- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd,
                      uint32_t size) {
  close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
  if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running = 0;

  if (key == KEY_ENTER && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (mode == MODE_TEXT)
      reading_pending = 1;
    else if (mode == MODE_SHOT)
      capture_pending = 1;
  }
  if (key == KEY_TAB && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    mode = mode == MODE_SHOT ? MODE_TEXT : MODE_SHOT;
    request_redraw();
  }
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial,
                    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_mods,
};

/* ---------- seat ---------- */
static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
  static struct wl_keyboard *kb;
  static struct wl_pointer *ptr;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) {
    kb = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(kb, &kb_listener, NULL);
  }
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
    ptr = wl_seat_get_pointer(s);
    wl_pointer_add_listener(ptr, &pointer_listener, NULL);
  }
}
static const struct wl_seat_listener seat_listener = {.capabilities =
                                                          seat_caps};

/* ---------- registry ---------- */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  if (strcmp(iface, wl_compositor_interface.name) == 0)
    compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (strcmp(iface, wl_shm_interface.name) == 0)
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (strcmp(iface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    wl_seat_add_listener(seat, &seat_listener, NULL);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
    layer_shell =
        wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ---------- cursor ---------- */
static void setup_cursor(void) {
  const char *theme = getenv("XCURSOR_THEME"); /* NULL = default theme */
  const char *size_env = getenv("XCURSOR_SIZE");
  int size = size_env ? atoi(size_env) : 24;
  if (size <= 0)
    size = 24;

  cursor_theme = wl_cursor_theme_load(theme, size, shm);
  if (!cursor_theme)
    return;
  struct wl_cursor *cursor =
      wl_cursor_theme_get_cursor(cursor_theme, "crosshair");
  if (!cursor)
    cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
  if (!cursor)
    return;

  cursor_image = cursor->images[0];
  cursor_surface = wl_compositor_create_surface(compositor);
  wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0,
                    0);
  wl_surface_damage_buffer(cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(cursor_surface);
}

int main(void) {
  display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "Can't connect to Wayland\n");
    return 1;
  }

  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  wl_display_roundtrip(display);

  if (!compositor || !shm || !layer_shell) {
    fprintf(stderr,
            "Compositor lacks wlr-layer-shell (GNOME doesn't support it)\n");
    return 1;
  }

  setup_cursor();

  surface = wl_compositor_create_surface(compositor);
  struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "overlay-demo");
  zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(ls, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(ls, 1);
  zwlr_layer_surface_v1_add_listener(ls, &layer_listener, NULL);
  wl_surface_commit(surface);

  while (running && wl_display_dispatch(display) != -1) {
    if (capture_pending) {
      capture_pending = 0;
      capture_box(theBox);
      running = 0;
    }
    if (reading_pending) {
      reading_pending = 0;
      hide_overlay();
      run_grim(theBox, OCR_PATH);
      run_ocr(OCR_PATH, theBox);
      mode = MODE_TEXT;
      show_overlay();
    }
  }

  /* print theBox in slurp's format: x,y wxh */
  printf("%d,%d %dx%d\n", (int)theBox.x, (int)theBox.y, (int)theBox.w,
         (int)theBox.h);

  if (frame_cb)
    wl_callback_destroy(frame_cb);
  destroy_buffers();
  zwlr_layer_surface_v1_destroy(ls);
  wl_surface_destroy(surface);
  if (cursor_surface)
    wl_surface_destroy(cursor_surface);
  if (cursor_theme)
    wl_cursor_theme_destroy(cursor_theme);
  wl_display_disconnect(display);
  return 0;
}
