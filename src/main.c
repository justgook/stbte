/* ==========================================================================
 * stb_tilemap_editor WASM Plugin
 *
 * Wraps Sean Barrett's stb_tilemap_editor.h as a WASM module that
 * communicates with the host via shared memory command/event buffers.
 *
 * Architecture:
 *   - WASM runs in a dedicated Web Worker
 *   - Draw commands (STBTE_DRAW_RECT/TILE) append to a command buffer
 *   - Mouse/keyboard events arrive via an event ring buffer
 *   - Double-buffered command output for lock-free rendering
 *   - All buffers live in WASM linear memory (SharedArrayBuffer)
 *
 * Build: zig build-exe main.c -target wasm32-freestanding -fno-entry
 *        -rdynamic -O ReleaseFast -femit-bin=stb_tilemap_editor.wasm
 * ==========================================================================
 */

#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * 1. FREESTANDING LIBC STUBS
 *
 * stb_tilemap_editor.h needs malloc (once), sprintf (for UI numbers),
 * and assert. We provide minimal implementations.
 * ========================================================================== */

/* ---- Memory ---- */

/* Simple bump allocator. stb_tilemap_editor only calls malloc() once in
 * stbte_create_map(), so we just need a single large allocation. */
static uint8_t heap_storage[8 * 1024 * 1024]
    __attribute__((aligned(16))); /* 8 MB heap */
static uint32_t heap_offset = 0;

void *malloc(size_t size) {
  /* Align to 16 bytes */
  uint32_t aligned = (heap_offset + 15) & ~15u;
  if (aligned + size > sizeof(heap_storage))
    return (void *)0;
  void *ptr = &heap_storage[aligned];
  heap_offset = aligned + (uint32_t)size;
  return ptr;
}

void free(void *ptr) { (void)ptr; /* no-op: stb never frees */ }

/* ---- String utilities ---- */

static void *memset(void *dest, int c, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  for (size_t i = 0; i < n; i++)
    d[i] = (uint8_t)c;
  return dest;
}

static void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
  return dest;
}

static size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

/* Minimal sprintf supporting: %d, %*d, %f, %6.2f, %8.4f, %s, %2d, %% */
static int sprintf(char *buf, const char *fmt, ...) {
  /* We use a varargs-free approach: stb_tilemap_editor's sprintf calls are
   * all via the stbte__sprintf macro with known patterns:
   *   - "%*d" with (digits, val) for info panel numbers
   *   - "%6.2f" or "%8.4f" for float property display
   *   - "%2d" for layer numbers
   * We handle these specific patterns. */

  /* This is a simplified approach - we parse the format string and handle
   * the specific patterns stb uses. We use __builtin_va_list since
   * stdarg.h may not be available in freestanding. */
  __builtin_va_list args;
  __builtin_va_start(args, fmt);

  char *out = buf;
  while (*fmt) {
    if (*fmt != '%') {
      *out++ = *fmt++;
      continue;
    }
    fmt++; /* skip '%' */

    if (*fmt == '%') {
      *out++ = '%';
      fmt++;
      continue;
    }

    /* Parse flags/width/precision */
    int width = 0;
    int precision = -1;
    int star_width = 0;

    /* Check for '*' width */
    if (*fmt == '*') {
      star_width = 1;
      width = __builtin_va_arg(args, int);
      fmt++;
    } else {
      /* Parse numeric width */
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }
    }

    /* Parse precision */
    if (*fmt == '.') {
      fmt++;
      precision = 0;
      while (*fmt >= '0' && *fmt <= '9') {
        precision = precision * 10 + (*fmt - '0');
        fmt++;
      }
    }

    /* Format specifier */
    switch (*fmt) {
    case 'd': {
      int val = __builtin_va_arg(args, int);
      char tmp[16];
      int neg = 0;
      unsigned int uval;
      if (val < 0) {
        neg = 1;
        uval = (unsigned int)(-(val + 1)) + 1;
      } else {
        uval = (unsigned int)val;
      }
      int len = 0;
      if (uval == 0) {
        tmp[len++] = '0';
      } else {
        while (uval > 0) {
          tmp[len++] = '0' + (uval % 10);
          uval /= 10;
        }
      }
      /* Add padding spaces */
      int total = len + neg;
      while (total < width) {
        *out++ = ' ';
        total++;
      }
      if (neg)
        *out++ = '-';
      for (int i = len - 1; i >= 0; i--)
        *out++ = tmp[i];
      fmt++;
      break;
    }
    case 'f': {
      double val = __builtin_va_arg(args, double);
      if (precision < 0)
        precision = 2;
      /* Format float */
      if (val < 0) {
        *out++ = '-';
        val = -val;
      }
      int int_part = (int)val;
      double frac = val - (double)int_part;

      /* Integer part */
      char tmp[16];
      int len = 0;
      if (int_part == 0) {
        tmp[len++] = '0';
      } else {
        unsigned int ui = (unsigned int)int_part;
        while (ui > 0) {
          tmp[len++] = '0' + (ui % 10);
          ui /= 10;
        }
      }
      /* Width padding (total width includes decimal point and precision) */
      int total_len = len + 1 + precision + (val < 0 ? 1 : 0);
      while (total_len < width) {
        *out++ = ' ';
        total_len++;
      }
      for (int i = len - 1; i >= 0; i--)
        *out++ = tmp[i];

      *out++ = '.';
      for (int i = 0; i < precision; i++) {
        frac *= 10.0;
        int digit = (int)frac;
        *out++ = '0' + digit;
        frac -= digit;
      }
      fmt++;
      break;
    }
    case 's': {
      const char *s = __builtin_va_arg(args, const char *);
      if (s) {
        while (*s)
          *out++ = *s++;
      }
      fmt++;
      break;
    }
    default:
      *out++ = *fmt++;
      break;
    }
  }

  __builtin_va_end(args);
  *out = '\0';
  return (int)(out - buf);
}

/* ---- Assert ---- */
#define STBTE_ASSERT(x) ((void)0)

/* assert function for stb's #include <assert.h> */
#define assert(x) ((void)0)

/* ==========================================================================
 * 2. COMMAND BUFFER SYSTEM
 *
 * Draw commands are appended to a fixed-size buffer during stbte_draw().
 * The host reads this buffer to replay drawing on Canvas2D.
 * ========================================================================== */

/* Command types */
#define CMD_RECT 1
#define CMD_TILE 2
#define CMD_END 0

/* Command buffer: array of uint32_t values.
 * CMD_RECT: [CMD_RECT, x0|y0<<16, x1|y1<<16, color]   = 4 words (packed coords)
 * CMD_TILE: [CMD_TILE, x0|y0<<16, tile_id, highlight]  = 4 words (packed
 * coords) CMD_END:  [CMD_END]                                   = 1 word
 * (sentinel)
 */

#define CMD_BUF_WORDS (64 * 1024) /* 256 KB per buffer */
static uint32_t cmd_buf_a[CMD_BUF_WORDS];
static uint32_t cmd_buf_b[CMD_BUF_WORDS];
static uint32_t cmd_count_a = 0;
static uint32_t cmd_count_b = 0;

/* Which buffer is currently being written to (0=A, 1=B) */
static uint32_t active_write_buf = 0;

static uint32_t *cmd_current_buf(void) {
  return active_write_buf == 0 ? cmd_buf_a : cmd_buf_b;
}

static uint32_t *cmd_current_count(void) {
  return active_write_buf == 0 ? &cmd_count_a : &cmd_count_b;
}

static void cmd_reset(void) { *cmd_current_count() = 0; }

static void cmd_push_rect(int x0, int y0, int x1, int y1, unsigned int color) {
  uint32_t *count = cmd_current_count();
  uint32_t *buf = cmd_current_buf();
  if (*count + 6 >= CMD_BUF_WORDS)
    return; /* overflow protection */
  uint32_t i = *count;
  buf[i + 0] = CMD_RECT;
  buf[i + 1] = (uint32_t)(x0 & 0xFFFF) | ((uint32_t)(y0 & 0xFFFF) << 16);
  buf[i + 2] = (uint32_t)(x1 & 0xFFFF) | ((uint32_t)(y1 & 0xFFFF) << 16);
  buf[i + 3] = color;
  *count = i + 4;
}

/* Tile ID fixup table: maps palette slot -> real tile ID.
 * Populated after stb_tilemap_editor.h is included and tiles are defined.
 * Used to convert imgui hit-test IDs to real tile IDs in DRAW_TILE. */
#define MAX_TILE_SLOTS 256
static uint16_t tile_slot_to_id[MAX_TILE_SLOTS];
static int tile_slot_count = 0;

static void cmd_push_tile(int x0, int y0, unsigned short id, int highlight,
                          float *data) {
  uint32_t *count = cmd_current_count();
  uint32_t *buf = cmd_current_buf();
  if (*count + 5 >= CMD_BUF_WORDS)
    return; /* overflow protection */

  /* stb_tilemap_editor passes imgui hit-test IDs for palette tiles instead
   * of the real tile ID. The palette ID has the form: STBTE__palette + (slot <<
   * 7) where STBTE__palette = 7. We detect this and convert to the real tile
   * ID. */
  uint32_t real_id = (uint32_t)id;
  if (data == NULL && (id & 0x7F) == 7) {
    /* Palette tile: decode slot and look up real tile ID */
    int slot = (id - 7) >> 7;
    if (slot >= 0 && slot < tile_slot_count) {
      real_id = (uint32_t)tile_slot_to_id[slot];
    }
  }

  uint32_t i = *count;
  buf[i + 0] = CMD_TILE;
  buf[i + 1] = (uint32_t)(x0 & 0xFFFF) | ((uint32_t)(y0 & 0xFFFF) << 16);
  buf[i + 2] = real_id;
  buf[i + 3] = (uint32_t)(highlight + 1); /* shift: -1->0, 0->1, 1->2 */
  (void)data;                             /* TODO phase 2: pass property data */
  *count = i + 4;
}

/* ==========================================================================
 * 3. EVENT RING BUFFER
 *
 * The host writes mouse/keyboard events here; WASM reads them in frame().
 * Ring buffer with head/tail pointers (Atomics-compatible uint32 values).
 * ========================================================================== */

/* Event types */
#define EVT_MOUSE_MOVE 1   /* x, y, shifted, scrollkey */
#define EVT_MOUSE_BUTTON 2 /* x, y, right, down, shifted, scrollkey */
#define EVT_MOUSE_WHEEL 3  /* x, y, vscroll */
#define EVT_ACTION 4       /* action_id */
#define EVT_RESIZE 5       /* x0, y0, x1, y1 */

#define EVT_BUF_WORDS (4 * 1024) /* 16 KB ring buffer */
static uint32_t evt_buf[EVT_BUF_WORDS];
static volatile uint32_t evt_head = 0; /* written by host */
static volatile uint32_t evt_tail = 0; /* read by WASM */

/* ==========================================================================
 * 4. CONTROL BLOCK
 *
 * Shared state between host and WASM, laid out for Atomics access.
 * ========================================================================== */

typedef struct {
  volatile uint32_t active_read_buf; /* 0: read A, 1: read B */
  volatile uint32_t cmd_count_a;
  volatile uint32_t cmd_count_b;
  volatile uint32_t evt_head;      /* mirror of evt_head for Atomics */
  volatile uint32_t evt_tail;      /* mirror of evt_tail for Atomics */
  volatile uint32_t frame_ready;   /* 1 when a new frame is available */
  volatile uint32_t editor_width;  /* current display width */
  volatile uint32_t editor_height; /* current display height */
  volatile uint32_t initialized;   /* 1 after init() succeeds */
  volatile uint32_t map_width;
  volatile uint32_t map_height;
  volatile uint32_t num_layers;
  volatile uint32_t padding[4]; /* align to 64 bytes */
} control_block_t;

static control_block_t control_block;

/* ==========================================================================
 * 5. STB TILEMAP EDITOR CONFIGURATION AND INCLUSION
 * ========================================================================== */

/* Configurable via compile-time defines, with sensible defaults */
#ifndef STBTE_MAX_TILEMAP_X
#define STBTE_MAX_TILEMAP_X 200
#endif

#ifndef STBTE_MAX_TILEMAP_Y
#define STBTE_MAX_TILEMAP_Y 200
#endif

#ifndef STBTE_MAX_LAYERS
#define STBTE_MAX_LAYERS 8
#endif

#ifndef STBTE_UNDO_BUFFER_BYTES
#define STBTE_UNDO_BUFFER_BYTES (1 << 22) /* 4 MB */
#endif

#ifndef STBTE_MAX_PROPERTIES
#define STBTE_MAX_PROPERTIES 10
#endif

#ifndef STBTE_MAX_CATEGORIES
#define STBTE_MAX_CATEGORIES 100
#endif

#ifndef STBTE_MAX_COPY
#define STBTE_MAX_COPY 16384 /* ~128x128 */
#endif

/* Wire draw callbacks to our command buffer */
#define STBTE_DRAW_RECT(x0, y0, x1, y1, color)                                 \
  cmd_push_rect(x0, y0, x1, y1, color)
#define STBTE_DRAW_TILE(x0, y0, id, highlight, data)                           \
  cmd_push_tile(x0, y0, id, highlight, data)

/* Prevent stb from including stdlib/stdio (we're freestanding) */
#ifdef _WIN32
#undef _WIN32
#endif

#define STB_TILEMAP_EDITOR_IMPLEMENTATION
#include "stb_tilemap_editor.h"

/* ==========================================================================
 * 6. EDITOR STATE
 * ========================================================================== */

static stbte_tilemap *tilemap = NULL;
static float last_frame_time = 0.0f;
static int display_set = 0;

/* ==========================================================================
 * 7. EVENT PROCESSING
 *
 * Read events from the ring buffer and forward them to stb.
 * ========================================================================== */

static void process_events(void) {
  if (tilemap == NULL)
    return;

  /* Read events from ring buffer using our local copy.
   * The host writes to control_block.evt_head via Atomics. */
  uint32_t head = control_block.evt_head;
  uint32_t tail = evt_tail;

  while (tail != head) {
    uint32_t type = evt_buf[tail % EVT_BUF_WORDS];
    tail++;

    switch (type) {
    case EVT_MOUSE_MOVE: {
      int x = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int y = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int shifted = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int scrollkey = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      stbte_mouse_move(tilemap, x, y, shifted, scrollkey);
      break;
    }
    case EVT_MOUSE_BUTTON: {
      int x = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int y = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int right = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int down = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int shifted = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int scrollkey = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      stbte_mouse_button(tilemap, x, y, right, down, shifted, scrollkey);
      break;
    }
    case EVT_MOUSE_WHEEL: {
      int x = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int y = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int vscroll = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      stbte_mouse_wheel(tilemap, x, y, vscroll);
      break;
    }
    case EVT_ACTION: {
      int action = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      stbte_action(tilemap, (enum stbte_action)action);
      break;
    }
    case EVT_RESIZE: {
      int x0 = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int y0 = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int x1 = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      int y1 = (int)evt_buf[tail % EVT_BUF_WORDS];
      tail++;
      stbte_set_display(x0, y0, x1, y1);
      control_block.editor_width = (uint32_t)(x1 - x0);
      control_block.editor_height = (uint32_t)(y1 - y0);
      display_set = 1;
      break;
    }
    }
  }

  evt_tail = tail;
  control_block.evt_tail = tail;
}

/* ==========================================================================
 * 8. EXPORTED FUNCTIONS
 * ========================================================================== */

/* Initialize the editor with a new tilemap.
 * Call after loading WASM but before frame().
 * Parameters are read from the control block (set by host before calling). */
__attribute__((export_name("init"))) uint32_t init(void) {
  /* Read configuration from control block, use defaults if 0 */
  uint32_t map_x = control_block.map_width;
  uint32_t map_y = control_block.map_height;
  uint32_t layers = control_block.num_layers;

  if (map_x == 0)
    map_x = 16;
  if (map_y == 0)
    map_y = 16;
  if (layers == 0)
    layers = 2;

  if (map_x > STBTE_MAX_TILEMAP_X)
    map_x = STBTE_MAX_TILEMAP_X;
  if (map_y > STBTE_MAX_TILEMAP_Y)
    map_y = STBTE_MAX_TILEMAP_Y;
  if (layers > STBTE_MAX_LAYERS)
    layers = STBTE_MAX_LAYERS;

  /* Default tile spacing (16x16 pixels) */
  int spacing_x = 16;
  int spacing_y = 16;
  int max_tiles = 256;

  tilemap = stbte_create_map((int)map_x, (int)map_y, (int)layers, spacing_x,
                             spacing_y, max_tiles);
  if (tilemap == NULL) {
    return 1; /* allocation failed */
  }

  /* Set default display (will be overridden by EVT_RESIZE) */
  stbte_set_display(0, 0, 800, 600);
  display_set = 1;

  /* Register some placeholder tiles for testing */
  /* Users will call define_tile() to set up real tiles */
  stbte_define_tile(tilemap, 0, 0xFF, "default");
  stbte_define_tile(tilemap, 1, 0xFF, "default");
  stbte_define_tile(tilemap, 2, 0xFF, "default");
  stbte_define_tile(tilemap, 3, 0xFF, "terrain");
  stbte_define_tile(tilemap, 4, 0xFF, "terrain");
  stbte_define_tile(tilemap, 5, 0xFF, "objects");

  /* Populate tile slot -> real ID lookup table */
  tile_slot_count = tilemap->num_tiles;
  for (int i = 0; i < tile_slot_count && i < MAX_TILE_SLOTS; i++) {
    tile_slot_to_id[i] = tilemap->tiles[i].id;
  }

  /* Set tile 0 as background */
  stbte_set_background_tile(tilemap, 0);

  control_block.map_width = map_x;
  control_block.map_height = map_y;
  control_block.num_layers = layers;
  control_block.initialized = 1;

  return 0;
}

/* Run one frame: process events, tick, draw.
 * Called repeatedly by the worker's setInterval. */
__attribute__((export_name("frame"))) uint32_t frame(void) {
  if (tilemap == NULL || !display_set)
    return 1;

  /* Process pending input events */
  process_events();

  /* Tick with fixed dt (~16ms = 60fps) */
  float dt = 1.0f / 60.0f;
  stbte_tick(tilemap, dt);

  /* Reset command buffer and draw */
  cmd_reset();
  stbte_draw(tilemap);

  /* Write command count to control block */
  if (active_write_buf == 0) {
    control_block.cmd_count_a = cmd_count_a;
  } else {
    control_block.cmd_count_b = cmd_count_b;
  }

  /* Swap buffers: tell host which buffer to read */
  control_block.active_read_buf = active_write_buf;
  control_block.frame_ready = 1;

  /* Switch to other buffer for next frame */
  active_write_buf = 1 - active_write_buf;

  return 0;
}

/* Define a new tile type.
 * Host writes tile info to a staging area, then calls this.
 * For simplicity, we use the PDK input mechanism. */
__attribute__((export_name("define_tile"))) uint32_t define_tile_export(void) {
  /* Input format: 4 bytes id (uint16) + 4 bytes layermask (uint32) +
   * null-terminated category string */
  /* For now, tiles are defined in init(). This will be expanded in Phase 2. */
  return 0;
}

/* Get pointer to command buffer A (for SharedArrayBuffer mapping) */
__attribute__((export_name("get_cmd_buf_a_ptr"))) uint32_t
get_cmd_buf_a_ptr(void) {
  return (uint32_t)(uintptr_t)cmd_buf_a;
}

/* Get pointer to command buffer B */
__attribute__((export_name("get_cmd_buf_b_ptr"))) uint32_t
get_cmd_buf_b_ptr(void) {
  return (uint32_t)(uintptr_t)cmd_buf_b;
}

/* Get pointer to event ring buffer */
__attribute__((export_name("get_event_buf_ptr"))) uint32_t
get_event_buf_ptr(void) {
  return (uint32_t)(uintptr_t)evt_buf;
}

/* Get pointer to control block */
__attribute__((export_name("get_control_block_ptr"))) uint32_t
get_control_block_ptr(void) {
  return (uint32_t)(uintptr_t)&control_block;
}

/* Get pointer to event head (for host Atomics.store) */
__attribute__((export_name("get_evt_head_ptr"))) uint32_t
get_evt_head_ptr(void) {
  return (uint32_t)(uintptr_t)&control_block.evt_head;
}

/* Get size of command buffer in uint32 words */
__attribute__((export_name("get_cmd_buf_size"))) uint32_t
get_cmd_buf_size(void) {
  return CMD_BUF_WORDS;
}

/* Get size of event buffer in uint32 words */
__attribute__((export_name("get_evt_buf_size"))) uint32_t
get_evt_buf_size(void) {
  return EVT_BUF_WORDS;
}

/* Get size of control block in bytes */
__attribute__((export_name("get_control_block_size"))) uint32_t
get_control_block_size(void) {
  return (uint32_t)sizeof(control_block_t);
}
