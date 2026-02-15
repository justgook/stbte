/* ==========================================================================
 * stb_tilemap_editor Headless WASM API
 *
 * Wraps Sean Barrett's stb_tilemap_editor.h as a headless WASM module.
 * All state is exposed via pointers for direct memory access from JS.
 * No properties or links - handled externally.
 *
 * Build: zig build-exe main.c -target wasm32-freestanding -fno-entry
 *        -rdynamic -O ReleaseFast -femit-bin=stb_tilemap_editor.wasm
 * ==========================================================================
 */

#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * 1. FREESTANDING LIBC STUBS
 * ========================================================================== */

static uint8_t heap_storage[16 * 1024 * 1024]
    __attribute__((aligned(16)));
static uint32_t heap_offset = 0;

__attribute__((export_name("malloc")))
void *malloc(size_t size) {
  uint32_t aligned = (heap_offset + 15) & ~15u;
  if (aligned + size > sizeof(heap_storage))
    return (void *)0;
  void *ptr = &heap_storage[aligned];
  heap_offset = aligned + (uint32_t)size;
  return ptr;
}

__attribute__((export_name("free")))
void free(void *ptr) { (void)ptr; }

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

// Disable properties and links - handled externally
#define STBTE_MAX_PROPERTIES 0
#undef STBTE_ALLOW_LINK

// Default limits (can be overridden at compile time)
#ifndef STBTE_MAX_TILEMAP_X
#define STBTE_MAX_TILEMAP_X 512
#endif

#ifndef STBTE_MAX_TILEMAP_Y
#define STBTE_MAX_TILEMAP_Y 512
#endif

#ifndef STBTE_MAX_LAYERS
#define STBTE_MAX_LAYERS 8
#endif

#ifndef STBTE_MAX_CATEGORIES
#define STBTE_MAX_CATEGORIES 100
#endif

#ifndef STBTE_UNDO_BUFFER_BYTES
#define STBTE_UNDO_BUFFER_BYTES (1 << 22)
#endif

#ifndef STBTE_MAX_COPY
#define STBTE_MAX_COPY 65536
#endif

// Stub out draw callbacks (not used in headless mode)
#define STBTE_DRAW_RECT(x0, y0, x1, y1, color) ((void)0)
#define STBTE_DRAW_TILE(x0, y0, id, highlight, data) ((void)0)

// Prevent stdlib includes
#ifdef _WIN32
#undef _WIN32
#endif

#define STB_TILEMAP_EDITOR_IMPLEMENTATION
#include "stb_tilemap_editor.h"

/* ==========================================================================
 * 3. EXPORTED STRUCTURES (for direct memory access)
 * ========================================================================== */

// Tool enum for reference
typedef enum {
  STBTE_TOOL_SELECT = 0,
  STBTE_TOOL_BRUSH = 1,
  STBTE_TOOL_ERASE = 2,
  STBTE_TOOL_RECTANGLE = 3,
  STBTE_TOOL_EYEDROPPER = 4,
  STBTE_TOOL_FILL = 5,
  STBTE_TOOL_LINK = 6,
} stbte_tool_t;

// Layer state
typedef struct {
  const char *name;
  int locked;    // 0=unlocked, 1=protected, 2=locked
  int hidden;
  int solo;
} stbte_layer_state_t;

// Tile info (read-only from JS)
typedef struct {
  unsigned short id;
  unsigned int layermask;
  const char *category;
  int category_id;
} stbte_tile_info_t;

// Clipboard info
typedef struct {
  int has_copy;
  int width;
  int height;
  int src_x;
  int src_y;
} stbte_clipboard_info_t;

// Selection info
typedef struct {
  int has_selection;
  int x0, y0, x1, y1;
} stbte_selection_info_t;

/* ==========================================================================
 * 4. INTERNAL STATE ACCESS HELPERS
 * ========================================================================== */

// Access internal UI state
static stbte__ui_t* get_ui(void) {
  return &stbte__ui;
}

// Convert internal tool enum to our tool enum
static int tool_from_internal(int internal_tool) {
  switch (internal_tool) {
    case STBTE__tool_select: return STBTE_TOOL_SELECT;
    case STBTE__tool_brush: return STBTE_TOOL_BRUSH;
    case STBTE__tool_erase: return STBTE_TOOL_ERASE;
    case STBTE__tool_rect: return STBTE_TOOL_RECTANGLE;
    case STBTE__tool_eyedrop: return STBTE_TOOL_EYEDROPPER;
    case STBTE__tool_fill: return STBTE_TOOL_FILL;
    case STBTE__tool_link: return STBTE_TOOL_LINK;
    default: return STBTE_TOOL_BRUSH;
  }
}

static int tool_to_internal(int tool) {
  switch (tool) {
    case STBTE_TOOL_SELECT: return STBTE__tool_select;
    case STBTE_TOOL_BRUSH: return STBTE__tool_brush;
    case STBTE_TOOL_ERASE: return STBTE__tool_erase;
    case STBTE_TOOL_RECTANGLE: return STBTE__tool_rect;
    case STBTE_TOOL_EYEDROPPER: return STBTE__tool_eyedrop;
    case STBTE_TOOL_FILL: return STBTE__tool_fill;
    case STBTE_TOOL_LINK: return STBTE__tool_link;
    default: return STBTE__tool_brush;
  }
}

/* ==========================================================================
 * 5. TILEMAP LIFECYCLE
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
  // stbte doesn't have a destroy function - just let it leak for now
  (void)tm;
}

__attribute__((export_name("stbte_clear"))) 
void stbte_clear(stbte_tilemap* tm) {
  stbte_clear_map(tm);
}

__attribute__((export_name("stbte_set_dimensions"))) 
void stbte_set_dims(stbte_tilemap* tm, int max_x, int max_y) {
  stbte_set_dimensions(tm, max_x, max_y);
}

__attribute__((export_name("stbte_get_dimensions"))) 
void stbte_get_dims(stbte_tilemap* tm, int* max_x, int* max_y) {
  stbte_get_dimensions(tm, max_x, max_y);
}

__attribute__((export_name("stbte_set_spacing"))) 
void stbte_set_space(stbte_tilemap* tm, int spacing_x, int spacing_y) {
  stbte_set_spacing(tm, spacing_x, spacing_y, spacing_x + 1, spacing_y + 1);
}

/* ==========================================================================
 * 6. TOOL MANAGEMENT
 * ========================================================================== */

__attribute__((export_name("stbte_set_tool"))) 
void stbte_set_current_tool(stbte_tilemap* tm, int tool) {
  (void)tm;
  stbte__ui.tool = tool_to_internal(tool);
  stbte__ui.has_selection = 0;
}

__attribute__((export_name("stbte_get_tool"))) 
int stbte_get_current_tool(stbte_tilemap* tm) {
  (void)tm;
  return tool_from_internal(stbte__ui.tool);
}

/* ==========================================================================
 * 7. ACTIVE TILE (BRUSH)
 * ========================================================================== */

__attribute__((export_name("stbte_set_active_tile"))) 
void stbte_set_brush_tile(stbte_tilemap* tm, int tile_index) {
  if (tile_index >= 0 && tile_index < tm->num_tiles) {
    tm->cur_tile = tile_index;
  }
}

__attribute__((export_name("stbte_get_active_tile"))) 
int stbte_get_brush_tile(stbte_tilemap* tm) {
  return tm->cur_tile;
}

/* ==========================================================================
 * 8. LAYER MANAGEMENT
 * ========================================================================== */

__attribute__((export_name("stbte_get_layer_count"))) 
int stbte_get_num_layers(stbte_tilemap* tm) {
  return tm->num_layers;
}

__attribute__((export_name("stbte_set_layer_name"))) 
void stbte_set_layername_wrapper(stbte_tilemap* tm, int layer, const char* name) {
  stbte_set_layername(tm, layer, name);
}

__attribute__((export_name("stbte_get_layer_name"))) 
const char* stbte_get_layername(stbte_tilemap* tm, int layer) {
  if (layer >= 0 && layer < tm->num_layers) {
    return tm->layerinfo[layer].name;
  }
  return NULL;
}

__attribute__((export_name("stbte_set_layer_hidden"))) 
void stbte_set_layer_hide(stbte_tilemap* tm, int layer, int hidden) {
  if (layer >= 0 && layer < tm->num_layers) {
    tm->layerinfo[layer].hidden = hidden ? 1 : 0;
  }
}

__attribute__((export_name("stbte_get_layer_hidden"))) 
int stbte_get_layer_hide(stbte_tilemap* tm, int layer) {
  if (layer >= 0 && layer < tm->num_layers) {
    return tm->layerinfo[layer].hidden;
  }
  return 0;
}

__attribute__((export_name("stbte_set_layer_locked"))) 
void stbte_set_layer_lock(stbte_tilemap* tm, int layer, int locked) {
  if (layer >= 0 && layer < tm->num_layers) {
    tm->layerinfo[layer].locked = locked % 3; // 0=unlocked, 1=protected, 2=locked
  }
}

__attribute__((export_name("stbte_get_layer_locked"))) 
int stbte_get_layer_lock(stbte_tilemap* tm, int layer) {
  if (layer >= 0 && layer < tm->num_layers) {
    return tm->layerinfo[layer].locked;
  }
  return 0;
}

__attribute__((export_name("stbte_set_active_layer"))) 
void stbte_set_cur_layer(stbte_tilemap* tm, int layer) {
  tm->cur_layer = layer;
}

__attribute__((export_name("stbte_get_active_layer"))) 
int stbte_get_cur_layer(stbte_tilemap* tm) {
  return tm->cur_layer;
}

__attribute__((export_name("stbte_set_solo_layer"))) 
void stbte_set_sololayer(stbte_tilemap* tm, int layer) {
  tm->solo_layer = layer;
}

__attribute__((export_name("stbte_get_solo_layer"))) 
int stbte_get_sololayer(stbte_tilemap* tm) {
  return tm->solo_layer;
}

/* ==========================================================================
 * 9. TILE DEFINITIONS
 * ========================================================================== */

__attribute__((export_name("stbte_define_tile"))) 
void stbte_add_tile(stbte_tilemap* tm, unsigned short id, unsigned int layermask, const char* category) {
  stbte_define_tile(tm, id, layermask, category);
}

__attribute__((export_name("stbte_get_tile_count"))) 
int stbte_get_num_tiles(stbte_tilemap* tm) {
  return tm->num_tiles;
}

__attribute__((export_name("stbte_get_tile_id"))) 
unsigned short stbte_get_tile_id(stbte_tilemap* tm, int index) {
  if (index >= 0 && index < tm->num_tiles) {
    return tm->tiles[index].id;
  }
  return 0;
}

__attribute__((export_name("stbte_get_tile_layermask"))) 
unsigned int stbte_get_tile_mask(stbte_tilemap* tm, int index) {
  if (index >= 0 && index < tm->num_tiles) {
    return tm->tiles[index].layermask;
  }
  return 0;
}

__attribute__((export_name("stbte_get_tile_category"))) 
const char* stbte_get_tile_cat(stbte_tilemap* tm, int index) {
  if (index >= 0 && index < tm->num_tiles) {
    return tm->tiles[index].category;
  }
  return NULL;
}

__attribute__((export_name("stbte_get_tile_category_id"))) 
int stbte_get_tile_catid(stbte_tilemap* tm, int index) {
  if (index >= 0 && index < tm->num_tiles) {
    // Make sure tileinfo is computed
    if (tm->tileinfo_dirty) {
      stbte__compute_tileinfo(tm);
    }
    return tm->tiles[index].category_id;
  }
  return -1;
}

/* ==========================================================================
 * 10. CATEGORIES
 * ========================================================================== */

__attribute__((export_name("stbte_get_category_count"))) 
int stbte_get_num_categories(stbte_tilemap* tm) {
  // Make sure tileinfo is computed
  if (tm->tileinfo_dirty) {
    stbte__compute_tileinfo(tm);
  }
  return tm->num_categories;
}

__attribute__((export_name("stbte_get_category_name"))) 
const char* stbte_get_cat_name(stbte_tilemap* tm, int index) {
  if (index >= 0 && index < tm->num_categories) {
    return tm->categories[index];
  }
  return NULL;
}

__attribute__((export_name("stbte_set_active_category"))) 
void stbte_set_cur_category(stbte_tilemap* tm, int category) {
  stbte__choose_category(tm, category);
}

__attribute__((export_name("stbte_get_active_category"))) 
int stbte_get_cur_category(stbte_tilemap* tm) {
  return tm->cur_category;
}

/* ==========================================================================
 * 11. SELECTION
 * ========================================================================== */

__attribute__((export_name("stbte_set_selection"))) 
void stbte_set_sel(stbte_tilemap* tm, int x0, int y0, int x1, int y1) {
  (void)tm;
  stbte__select_rect(tm, x0, y0, x1, y1);
}

__attribute__((export_name("stbte_get_selection"))) 
void stbte_get_sel(stbte_tilemap* tm, int* x0, int* y0, int* x1, int* y1) {
  (void)tm;
  *x0 = stbte__ui.select_x0;
  *y0 = stbte__ui.select_y0;
  *x1 = stbte__ui.select_x1;
  *y1 = stbte__ui.select_y1;
}

__attribute__((export_name("stbte_clear_selection"))) 
void stbte_clear_sel(stbte_tilemap* tm) {
  (void)tm;
  stbte__ui.has_selection = 0;
}

__attribute__((export_name("stbte_has_selection"))) 
int stbte_has_sel(stbte_tilemap* tm) {
  (void)tm;
  return stbte__ui.has_selection;
}

/* ==========================================================================
 * 12. CLIPBOARD (COPY/CUT/PASTE)
 * ========================================================================== */

__attribute__((export_name("stbte_copy"))) 
void stbte_copy_selection(stbte_tilemap* tm) {
  stbte__copy_cut(tm, 0);
}

__attribute__((export_name("stbte_cut"))) 
void stbte_cut_selection(stbte_tilemap* tm) {
  stbte__copy_cut(tm, 1);
}

__attribute__((export_name("stbte_paste"))) 
void stbte_paste_clipboard(stbte_tilemap* tm, int x, int y) {
  // stbte centers the paste on x,y
  stbte__paste(tm, x, y);
}

__attribute__((export_name("stbte_has_clipboard"))) 
int stbte_has_clip(stbte_tilemap* tm) {
  (void)tm;
  return stbte__ui.has_copy;
}

__attribute__((export_name("stbte_get_clipboard_info"))) 
void stbte_get_clip_info(stbte_tilemap* tm, int* width, int* height, int* src_x, int* src_y) {
  (void)tm;
  *width = stbte__ui.copy_width;
  *height = stbte__ui.copy_height;
  *src_x = stbte__ui.copy_src_x;
  *src_y = stbte__ui.copy_src_y;
}

/* ==========================================================================
 * 13. UNDO/REDO
 * ========================================================================== */

__attribute__((export_name("stbte_can_undo"))) 
int stbte_undo_available(stbte_tilemap* tm) {
  return stbte__undo_available(tm);
}

__attribute__((export_name("stbte_can_redo"))) 
int stbte_redo_available(stbte_tilemap* tm) {
  return stbte__redo_available(tm);
}

__attribute__((export_name("stbte_undo"))) 
void stbte_do_undo(stbte_tilemap* tm) {
  (void)tm;
  stbte__undo(tm);
}

__attribute__((export_name("stbte_redo"))) 
void stbte_do_redo(stbte_tilemap* tm) {
  (void)tm;
  stbte__redo(tm);
}

/* ==========================================================================
 * 14. TILE INTERACTION (CLICK/DRAG)
 * ========================================================================== */

__attribute__((export_name("stbte_click_tile"))) 
void stbte_click(stbte_tilemap* tm, int x, int y, int button) {
  int tool = stbte__ui.tool;
  
  // Ensure coordinates are valid
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return;
  
  stbte__begin_undo(tm);
  
  switch (tool) {
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
      // Set selection to single tile
      stbte__select_rect(tm, x, y, x, y);
      break;
      
    case STBTE__tool_rect:
      // Rectangle tool requires drag - single click fills just one tile
      if (button == 0) {
        stbte__brush(tm, x, y);
      } else {
        stbte__erase(tm, x, y, STBTE__erase_any);
      }
      break;
  }
  
  stbte__end_undo(tm);
}

__attribute__((export_name("stbte_fill_rect"))) 
void stbte_fill_rectangle(stbte_tilemap* tm, int x0, int y0, int x1, int y1, int fill) {
  stbte__fillrect(tm, x0, y0, x1, y1, fill);
}

/* ==========================================================================
 * 15. MAP DATA ACCESS (DIRECT POINTERS)
 * ========================================================================== */

// Get pointer to tile data at x,y - returns short[layers]
__attribute__((export_name("stbte_get_tile_ptr"))) 
short* stbte_get_tile_data(stbte_tilemap* tm, int x, int y) {
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return NULL;
  return tm->data[y][x];
}

// Set a single tile
__attribute__((export_name("stbte_set_tile"))) 
void stbte_set_tile_data(stbte_tilemap* tm, int x, int y, int layer, short tile_id) {
  if (x < 0 || x >= tm->max_x || y < 0 || y >= tm->max_y)
    return;
  if (layer < 0 || layer >= tm->num_layers)
    return;
  tm->data[y][x][layer] = tile_id;
}

// Get pointer to the entire map data array
__attribute__((export_name("stbte_get_map_data_ptr"))) 
short* stbte_get_map_data(stbte_tilemap* tm) {
  return (short*)tm->data;
}

// Get pointer to layer info array
__attribute__((export_name("stbte_get_layer_info_ptr"))) 
stbte__layer* stbte_get_layer_info(stbte_tilemap* tm) {
  return tm->layerinfo;
}

// Get pointer to tile info array
__attribute__((export_name("stbte_get_tile_info_ptr"))) 
stbte__tileinfo* stbte_get_tile_info(stbte_tilemap* tm) {
  return tm->tiles;
}

// Get pointer to UI state
__attribute__((export_name("stbte_get_ui_ptr"))) 
stbte__ui_t* stbte_get_ui_state(void) {
  return &stbte__ui;
}

// Get pointer to copy buffer
__attribute__((export_name("stbte_get_copy_buffer_ptr"))) 
short* stbte_get_copy_buffer(void) {
  return (short*)stbte__ui.copybuffer;
}

/* ==========================================================================
 * 16. DISPLAY OPTIONS
 * ========================================================================== */

__attribute__((export_name("stbte_set_show_grid"))) 
void stbte_set_grid(stbte_tilemap* tm, int show) {
  (void)tm;
  stbte__ui.show_grid = show;
}

__attribute__((export_name("stbte_get_show_grid"))) 
int stbte_get_grid(stbte_tilemap* tm) {
  (void)tm;
  return stbte__ui.show_grid;
}

__attribute__((export_name("stbte_set_background_tile"))) 
void stbte_set_bg_tile(stbte_tilemap* tm, short tile_id) {
  stbte_set_background_tile(tm, tile_id);
}

__attribute__((export_name("stbte_get_background_tile"))) 
short stbte_get_bg_tile(stbte_tilemap* tm) {
  return tm->background_tile;
}

/* ==========================================================================
 * 17. UTILITY / INFO
 * ========================================================================== */

__attribute__((export_name("stbte_get_max_tiles"))) 
int stbte_get_max_tiles_limit(void) {
  return STBTE_MAX_TILEMAP_X * STBTE_MAX_TILEMAP_Y;
}

__attribute__((export_name("stbte_get_max_layers"))) 
int stbte_get_max_layers_limit(void) {
  return STBTE_MAX_LAYERS;
}

__attribute__((export_name("stbte_get_max_copy"))) 
int stbte_get_max_copy_limit(void) {
  return STBTE_MAX_COPY;
}

// Get map dimensions limits
__attribute__((export_name("stbte_get_max_map_x"))) 
int stbte_get_max_map_x_limit(void) {
  return STBTE_MAX_TILEMAP_X;
}

__attribute__((export_name("stbte_get_max_map_y"))) 
int stbte_get_max_map_y_limit(void) {
  return STBTE_MAX_TILEMAP_Y;
}

/* ==========================================================================
 * 18. MEMORY OFFSETS (for JS struct access)
 * ========================================================================== */

__attribute__((export_name("stbte_get_offset_stbte_tilemap_max_x"))) 
int stbte_offset_max_x(void) {
  return (int)offsetof(stbte_tilemap, max_x);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_max_y"))) 
int stbte_offset_max_y(void) {
  return (int)offsetof(stbte_tilemap, max_y);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_num_layers"))) 
int stbte_offset_num_layers(void) {
  return (int)offsetof(stbte_tilemap, num_layers);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_num_tiles"))) 
int stbte_offset_num_tiles(void) {
  return (int)offsetof(stbte_tilemap, num_tiles);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_cur_tile"))) 
int stbte_offset_cur_tile(void) {
  return (int)offsetof(stbte_tilemap, cur_tile);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_cur_layer"))) 
int stbte_offset_cur_layer(void) {
  return (int)offsetof(stbte_tilemap, cur_layer);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_solo_layer"))) 
int stbte_offset_solo_layer(void) {
  return (int)offsetof(stbte_tilemap, solo_layer);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_background_tile"))) 
int stbte_offset_bg_tile(void) {
  return (int)offsetof(stbte_tilemap, background_tile);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_data"))) 
int stbte_offset_data(void) {
  return (int)offsetof(stbte_tilemap, data);
}

__attribute__((export_name("stbte_get_offset_stbte_tilemap_tiles"))) 
int stbte_offset_tiles(void) {
  return (int)offsetof(stbte_tilemap, tiles);
}

__attribute__((export_name("stbte_get_sizeof_stbte_tilemap"))) 
int stbte_sizeof_tilemap(void) {
  return (int)sizeof(stbte_tilemap);
}

__attribute__((export_name("stbte_get_sizeof_stbte__layer"))) 
int stbte_sizeof_layer(void) {
  return (int)sizeof(stbte__layer);
}

__attribute__((export_name("stbte_get_sizeof_stbte__tileinfo"))) 
int stbte_sizeof_tileinfo(void) {
  return (int)sizeof(stbte__tileinfo);
}

__attribute__((export_name("stbte_get_sizeof_stbte__ui_t"))) 
int stbte_sizeof_ui(void) {
  return (int)sizeof(stbte__ui_t);
}
