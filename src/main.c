/* ==========================================================================
 * stb_tilemap_editor Headless WASM API - Minimal
 *
 * Exports only action functions. All state accessed via memory pointers.
 * Host provides memory import. No names passed - work with indexes only.
 *
 * Build: zig build-exe main.c -target wasm32-freestanding -fno-entry
 *        -rdynamic -O ReleaseFast -femit-bin=stb_tilemap_editor.wasm
 * ==========================================================================
 */

#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * 1. FREESTANDING LIBC STUBS (internal use only)
 * ========================================================================== */

static uint8_t heap_storage[16 * 1024 * 1024]
    __attribute__((aligned(16)));
static uint32_t heap_offset = 0;

static void *malloc_internal(size_t size) {
  uint32_t aligned = (heap_offset + 15) & ~15u;
  if (aligned + size > sizeof(heap_storage))
    return (void *)0;
  void *ptr = &heap_storage[aligned];
  heap_offset = aligned + (uint32_t)size;
  return ptr;
}

static void free_internal(void *ptr) { (void)ptr; }

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
  while (s[len]) len++;
  return len;
}

#define STB_SPRINTF_STATIC
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"
#define sprintf stbsp_sprintf

#define STBTE_ASSERT(x) ((void)0)
#define assert(x) ((void)0)

/* ==========================================================================
 * 2. STB TILEMAP EDITOR CONFIGURATION
 * ========================================================================== */

// Disable properties and links
#define STBTE_MAX_PROPERTIES 0
#undef STBTE_ALLOW_LINK

// Reduced sizes for smaller WASM - adjust as needed
#ifndef STBTE_MAX_TILEMAP_X
#define STBTE_MAX_TILEMAP_X 64
#endif

#ifndef STBTE_MAX_TILEMAP_Y
#define STBTE_MAX_TILEMAP_Y 64
#endif

#ifndef STBTE_MAX_LAYERS
#define STBTE_MAX_LAYERS 4
#endif

#ifndef STBTE_MAX_CATEGORIES
#define STBTE_MAX_CATEGORIES 16
#endif

#ifndef STBTE_UNDO_BUFFER_BYTES
#define STBTE_UNDO_BUFFER_BYTES (1 << 18)  // 256KB
#endif

#ifndef STBTE_MAX_COPY
#define STBTE_MAX_COPY 4096
#endif

// Stub out draw callbacks (headless)
#define STBTE_DRAW_RECT(x0, y0, x1, y1, color) ((void)0)
#define STBTE_DRAW_TILE(x0, y0, id, highlight, data) ((void)0)

#ifdef _WIN32
#undef _WIN32
#endif

// Redirect malloc/free to our internal versions
#define malloc malloc_internal
#define free free_internal

#define STB_TILEMAP_EDITOR_IMPLEMENTATION
#include "stb_tilemap_editor.h"

/* ==========================================================================
 * 3. EXPORTED API - ACTION FUNCTIONS ONLY
 * ========================================================================== */

// Tool constants
#define STBTE_TOOL_SELECT 0
#define STBTE_TOOL_BRUSH 1
#define STBTE_TOOL_ERASE 2
#define STBTE_TOOL_RECTANGLE 3
#define STBTE_TOOL_EYEDROPPER 4
#define STBTE_TOOL_FILL 5
#define STBTE_TOOL_LINK 6

/* ==========================================================================
 * LIFECYCLE
 * ========================================================================== */

__attribute__((export_name("stbte_create"))) 
stbte_tilemap* stbte_create(int map_x, int map_y, int layers, int spacing_x, int spacing_y, int max_tiles) {
  if (!stbte__ui.initted) {
    stbte__init_gui();
  }
  return stbte_create_map(map_x, map_y, layers, spacing_x, spacing_y, max_tiles);
}

__attribute__((export_name("stbte_destroy"))) 
void stbte_destroy(stbte_tilemap* tm) {
  (void)tm;
}

__attribute__((export_name("stbte_clear"))) 
void stbte_clear(stbte_tilemap* tm) {
  stbte_clear_map(tm);
  // Reset undo state — clearing is not undoable
  tm->undo_len = 0;
  tm->redo_len = 0;
  tm->undo_pos = 0;
  tm->undo_available_valid = 0;
  stbte__recompute_undo_available(tm);
}

__attribute__((export_name("stbte_set_dimensions"))) 
void stbte_set_dims(stbte_tilemap* tm, int max_x, int max_y) {
  stbte_set_dimensions(tm, max_x, max_y);
}

__attribute__((export_name("stbte_set_spacing"))) 
void stbte_set_space(stbte_tilemap* tm, int spacing_x, int spacing_y) {
  stbte_set_spacing(tm, spacing_x, spacing_y, spacing_x + 1, spacing_y + 1);
}

/* ==========================================================================
 * TOOL MANAGEMENT
 * ========================================================================== */

__attribute__((export_name("stbte_set_tool"))) 
void stbte_set_current_tool(stbte_tilemap* tm, int tool) {
  (void)tm;
  switch (tool) {
    case STBTE_TOOL_SELECT:    stbte__ui.tool = STBTE__tool_select;  break;
    case STBTE_TOOL_BRUSH:     stbte__ui.tool = STBTE__tool_brush;   break;
    case STBTE_TOOL_ERASE:     stbte__ui.tool = STBTE__tool_erase;   break;
    case STBTE_TOOL_RECTANGLE: stbte__ui.tool = STBTE__tool_rect;    break;
    case STBTE_TOOL_EYEDROPPER:stbte__ui.tool = STBTE__tool_eyedrop; break;
    // Fill and Link tools not implemented in headless mode — ignored
  }
}

/* ==========================================================================
 * ACTIVE TILE (BRUSH)
 * ========================================================================== */

__attribute__((export_name("stbte_set_active_tile"))) 
void stbte_set_brush_tile(stbte_tilemap* tm, int tile_index) {
  if (tile_index >= 0 && tile_index < tm->num_tiles) {
    tm->cur_tile = tile_index;
  }
}

/* ==========================================================================
 * LAYER MANAGEMENT
 * ========================================================================== */

__attribute__((export_name("stbte_set_layer_hidden"))) 
void stbte_set_layer_hide(stbte_tilemap* tm, int layer, int hidden) {
  if (layer >= 0 && layer < tm->num_layers) {
    tm->layerinfo[layer].hidden = hidden ? 1 : 0;
  }
}

__attribute__((export_name("stbte_set_layer_locked"))) 
void stbte_set_layer_lock(stbte_tilemap* tm, int layer, int locked) {
  if (layer >= 0 && layer < tm->num_layers) {
    tm->layerinfo[layer].locked = (locked < 0) ? 0 : (locked > 2) ? 2 : locked;
  }
}

__attribute__((export_name("stbte_set_active_layer"))) 
void stbte_set_cur_layer(stbte_tilemap* tm, int layer) {
  if (layer >= -1 && layer < tm->num_layers)
    tm->cur_layer = layer;
}

__attribute__((export_name("stbte_set_solo_layer"))) 
void stbte_set_sololayer(stbte_tilemap* tm, int layer) {
  if (layer >= -1 && layer < tm->num_layers)
    tm->solo_layer = layer;
}

/* ==========================================================================
 * TILE DEFINITIONS
 * ========================================================================== */

// Pre-allocated category string table (avoids leaking 32 bytes per define_tile call)
static char category_strings[STBTE_MAX_CATEGORIES][8];
static int category_strings_initted = 0;

__attribute__((export_name("stbte_define_tile"))) 
void stbte_add_tile(stbte_tilemap* tm, unsigned short id, unsigned int layermask, int category_index) {
  if (!category_strings_initted) {
    for (int i = 0; i < STBTE_MAX_CATEGORIES; i++)
      sprintf(category_strings[i], "%d", i);
    category_strings_initted = 1;
  }
  if (category_index < 0 || category_index >= STBTE_MAX_CATEGORIES)
    return;
  stbte_define_tile(tm, id, layermask, category_strings[category_index]);
}

/* ==========================================================================
 * CATEGORY
 * ========================================================================== */

__attribute__((export_name("stbte_set_active_category"))) 
void stbte_set_cur_category(stbte_tilemap* tm, int category) {
  // Ensure tile info is computed (computes categories from tile definitions)
  stbte__prepare_tileinfo(tm);
  stbte__choose_category(tm, category);
}

/* ==========================================================================
 * SELECTION
 * ========================================================================== */

__attribute__((export_name("stbte_set_selection"))) 
void stbte_set_sel(stbte_tilemap* tm, int x0, int y0, int x1, int y1) {
  (void)tm;
  stbte__ui.has_selection = 1;
  stbte__ui.select_x0 = (x0 < x1 ? x0 : x1);
  stbte__ui.select_x1 = (x0 < x1 ? x1 : x0);
  stbte__ui.select_y0 = (y0 < y1 ? y0 : y1);
  stbte__ui.select_y1 = (y0 < y1 ? y1 : y0);
}

__attribute__((export_name("stbte_clear_selection"))) 
void stbte_clear_sel(stbte_tilemap* tm) {
  (void)tm;
  stbte__ui.has_selection = 0;
}

/* ==========================================================================
 * CLIPBOARD
 * ========================================================================== */

__attribute__((export_name("stbte_copy"))) 
void stbte_copy_selection(stbte_tilemap* tm) {
  stbte__copy_cut(tm, 0);
}

__attribute__((export_name("stbte_cut"))) 
void stbte_cut_selection(stbte_tilemap* tm) {
  stbte__copy_cut(tm, 1);
  stbte__recompute_undo_available(tm);
}

__attribute__((export_name("stbte_paste"))) 
void stbte_paste_clipboard(stbte_tilemap* tm, int x, int y) {
  stbte__paste(tm, x, y);
  stbte__recompute_undo_available(tm);
}

/* ==========================================================================
 * UNDO/REDO
 * ========================================================================== */

__attribute__((export_name("stbte_undo"))) 
void stbte_do_undo(stbte_tilemap* tm) {
  stbte__undo(tm);
  stbte__recompute_undo_available(tm);
}

__attribute__((export_name("stbte_redo"))) 
void stbte_do_redo(stbte_tilemap* tm) {
  stbte__redo(tm);
  stbte__recompute_undo_available(tm);
}

/* ==========================================================================
 * TILE INTERACTION
 * ========================================================================== */

__attribute__((export_name("stbte_click_tile"))) 
void stbte_click(stbte_tilemap* tm, int x, int y, int button) {
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return;
  
  stbte__begin_undo(tm);
  
  switch (stbte__ui.tool) {
    case STBTE__tool_brush:
      if (button == 0) {
        stbte__brush(tm, x, y);
      } else {
        stbte__erase(tm, x, y, STBTE__erase_any);
      }
      break;
      
    case STBTE__tool_erase:
      stbte__erase(tm, x, y, STBTE__erase_all);
      break;
      
    case STBTE__tool_eyedrop:
      if (button == 0) {
        stbte__eyedrop(tm, x, y);
      }
      break;
      
    case STBTE__tool_select:
      stbte__ui.has_selection = 1;
      stbte__ui.select_x0 = x;
      stbte__ui.select_y0 = y;
      stbte__ui.select_x1 = x;
      stbte__ui.select_y1 = y;
      break;
      
    case STBTE__tool_rect:
      if (button == 0) {
        stbte__brush(tm, x, y);
      } else {
        stbte__erase(tm, x, y, STBTE__erase_any);
      }
      break;
  }
  
  stbte__end_undo(tm);
  stbte__recompute_undo_available(tm);
}

__attribute__((export_name("stbte_fill_rect"))) 
void stbte_fill_rectangle(stbte_tilemap* tm, int x0, int y0, int x1, int y1, int fill) {
  stbte__fillrect(tm, x0, y0, x1, y1, fill);
  stbte__recompute_undo_available(tm);
}

__attribute__((export_name("stbte_select_rect"))) 
void stbte_select_rectangle(stbte_tilemap* tm, int x0, int y0, int x1, int y1) {
  (void)tm;
  stbte__select_rect(tm, x0, y0, x1, y1);
}

/* ==========================================================================
 * MAP DATA
 * ========================================================================== */

__attribute__((export_name("stbte_set_tile"))) 
void stbte_set_tile_data(stbte_tilemap* tm, int x, int y, int layer, short tile_id) {
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return;
  if (layer < 0 || layer >= tm->num_layers)
    return;
  tm->data[y][x][layer] = tile_id;
}

__attribute__((export_name("stbte_set_background_tile"))) 
void stbte_set_bg_tile(stbte_tilemap* tm, short tile_id) {
  stbte_set_background_tile(tm, tile_id);
}

/* ==========================================================================
 * GETTER FUNCTIONS
 * ========================================================================== */

__attribute__((export_name("stbte_get_tool")))
int stbte_get_current_tool(void) {
  switch (stbte__ui.tool) {
    case STBTE__tool_select:  return STBTE_TOOL_SELECT;
    case STBTE__tool_brush:   return STBTE_TOOL_BRUSH;
    case STBTE__tool_erase:   return STBTE_TOOL_ERASE;
    case STBTE__tool_rect:    return STBTE_TOOL_RECTANGLE;
    case STBTE__tool_eyedrop: return STBTE_TOOL_EYEDROPPER;
    case STBTE__tool_fill:    return STBTE_TOOL_FILL;
    case STBTE__tool_link:    return STBTE_TOOL_LINK;
    default:                  return -1;
  }
}

__attribute__((export_name("stbte_get_tile_id")))
int stbte_get_tile_at(stbte_tilemap* tm, int x, int y, int layer) {
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return -1;
  if (layer < 0 || layer >= tm->num_layers)
    return -1;
  return tm->data[y][x][layer];
}

/* ==========================================================================
 * 4. MEMORY LAYOUT EXPORT - Struct offsets for JS direct access
 * ========================================================================== */

// stbte_tilemap offsets
__attribute__((export_name("stbte_offset_tilemap_max_x"))) 
int stbte_offset_tm_max_x(void) { return offsetof(stbte_tilemap, max_x); }

__attribute__((export_name("stbte_offset_tilemap_max_y"))) 
int stbte_offset_tm_max_y(void) { return offsetof(stbte_tilemap, max_y); }

__attribute__((export_name("stbte_offset_tilemap_num_layers"))) 
int stbte_offset_tm_num_layers(void) { return offsetof(stbte_tilemap, num_layers); }

__attribute__((export_name("stbte_offset_tilemap_num_tiles"))) 
int stbte_offset_tm_num_tiles(void) { return offsetof(stbte_tilemap, num_tiles); }

__attribute__((export_name("stbte_offset_tilemap_cur_tile"))) 
int stbte_offset_tm_cur_tile(void) { return offsetof(stbte_tilemap, cur_tile); }

__attribute__((export_name("stbte_offset_tilemap_cur_layer"))) 
int stbte_offset_tm_cur_layer(void) { return offsetof(stbte_tilemap, cur_layer); }

__attribute__((export_name("stbte_offset_tilemap_solo_layer"))) 
int stbte_offset_tm_solo_layer(void) { return offsetof(stbte_tilemap, solo_layer); }

__attribute__((export_name("stbte_offset_tilemap_cur_category"))) 
int stbte_offset_tm_cur_category(void) { return offsetof(stbte_tilemap, cur_category); }

__attribute__((export_name("stbte_offset_tilemap_background_tile"))) 
int stbte_offset_tm_bg_tile(void) { return offsetof(stbte_tilemap, background_tile); }

__attribute__((export_name("stbte_offset_tilemap_num_categories"))) 
int stbte_offset_tm_num_categories(void) { return offsetof(stbte_tilemap, num_categories); }

__attribute__((export_name("stbte_offset_tilemap_data"))) 
int stbte_offset_tm_data(void) { return offsetof(stbte_tilemap, data); }

__attribute__((export_name("stbte_offset_tilemap_tiles"))) 
int stbte_offset_tm_tiles(void) { return offsetof(stbte_tilemap, tiles); }

__attribute__((export_name("stbte_offset_tilemap_layerinfo"))) 
int stbte_offset_tm_layerinfo(void) { return offsetof(stbte_tilemap, layerinfo); }

// stbte__layer offsets
__attribute__((export_name("stbte_offset_layer_hidden"))) 
int stbte_offset_layer_hidden(void) { return offsetof(stbte__layer, hidden); }

__attribute__((export_name("stbte_offset_layer_locked"))) 
int stbte_offset_layer_locked(void) { return offsetof(stbte__layer, locked); }

// stbte__tileinfo offsets
__attribute__((export_name("stbte_offset_tileinfo_id"))) 
int stbte_offset_tileinfo_id(void) { return offsetof(stbte__tileinfo, id); }

__attribute__((export_name("stbte_offset_tileinfo_layermask"))) 
int stbte_offset_tileinfo_layermask(void) { return offsetof(stbte__tileinfo, layermask); }

__attribute__((export_name("stbte_offset_tileinfo_category_id"))) 
int stbte_offset_tileinfo_category_id(void) { return offsetof(stbte__tileinfo, category_id); }

// stbte__ui_t base address (static global — need absolute ptr for JS)
__attribute__((export_name("stbte_ui_ptr")))
int stbte_get_ui_ptr(void) { return (int)(uintptr_t)&stbte__ui; }

// stbte__ui_t offsets
__attribute__((export_name("stbte_offset_ui_tool"))) 
int stbte_offset_ui_tool(void) { return offsetof(stbte__ui_t, tool); }

__attribute__((export_name("stbte_offset_ui_has_selection"))) 
int stbte_offset_ui_has_selection(void) { return offsetof(stbte__ui_t, has_selection); }

__attribute__((export_name("stbte_offset_ui_select_x0"))) 
int stbte_offset_ui_select_x0(void) { return offsetof(stbte__ui_t, select_x0); }

__attribute__((export_name("stbte_offset_ui_select_y0"))) 
int stbte_offset_ui_select_y0(void) { return offsetof(stbte__ui_t, select_y0); }

__attribute__((export_name("stbte_offset_ui_select_x1"))) 
int stbte_offset_ui_select_x1(void) { return offsetof(stbte__ui_t, select_x1); }

__attribute__((export_name("stbte_offset_ui_select_y1"))) 
int stbte_offset_ui_select_y1(void) { return offsetof(stbte__ui_t, select_y1); }

__attribute__((export_name("stbte_offset_ui_has_copy"))) 
int stbte_offset_ui_has_copy(void) { return offsetof(stbte__ui_t, has_copy); }

__attribute__((export_name("stbte_offset_ui_copy_width"))) 
int stbte_offset_ui_copy_width(void) { return offsetof(stbte__ui_t, copy_width); }

__attribute__((export_name("stbte_offset_ui_copy_height"))) 
int stbte_offset_ui_copy_height(void) { return offsetof(stbte__ui_t, copy_height); }

__attribute__((export_name("stbte_offset_ui_copybuffer"))) 
int stbte_offset_ui_copybuffer(void) { return offsetof(stbte__ui_t, copybuffer); }

// stbte_tilemap - undo/redo availability (in tilemap, not UI)
__attribute__((export_name("stbte_offset_tilemap_undo_available"))) 
int stbte_offset_tm_undo_avail(void) { return offsetof(stbte_tilemap, undo_available); }

__attribute__((export_name("stbte_offset_tilemap_redo_available"))) 
int stbte_offset_tm_redo_avail(void) { return offsetof(stbte_tilemap, redo_available); }

// Struct sizes
__attribute__((export_name("stbte_sizeof_tilemap"))) 
int stbte_sizeof_tm(void) { return sizeof(stbte_tilemap); }

__attribute__((export_name("stbte_sizeof_layer"))) 
int stbte_sizeof_layer(void) { return sizeof(stbte__layer); }

__attribute__((export_name("stbte_sizeof_tileinfo"))) 
int stbte_sizeof_tileinfo(void) { return sizeof(stbte__tileinfo); }

__attribute__((export_name("stbte_sizeof_ui"))) 
int stbte_sizeof_ui(void) { return sizeof(stbte__ui_t); }

// Constants
__attribute__((export_name("stbte_max_map_x"))) 
int stbte_get_max_map_x(void) { return STBTE_MAX_TILEMAP_X; }

__attribute__((export_name("stbte_max_map_y"))) 
int stbte_get_max_map_y(void) { return STBTE_MAX_TILEMAP_Y; }

__attribute__((export_name("stbte_max_layers"))) 
int stbte_get_max_layers(void) { return STBTE_MAX_LAYERS; }

__attribute__((export_name("stbte_max_copy"))) 
int stbte_get_max_copy(void) { return STBTE_MAX_COPY; }

__attribute__((export_name("stbte_max_categories"))) 
int stbte_get_max_categories(void) { return STBTE_MAX_CATEGORIES; }
