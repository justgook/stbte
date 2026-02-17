// stb_tilemap_editor.h - v0.42 - Sean Barrett - http://nothings.org/stb
// placed in the public domain - not copyrighted - first released 2014-09
//
// Headless tilemap editor for C — UI stripped, state & actions only.
// Based on the original stb_tilemap_editor.h, refactored for WASM use.
//
// LICENSE
//
//   See end of file for license information.


///////////////////////////////////////////////////////////////////////
//
//   HEADER SECTION

#ifndef STB_TILEMAP_INCLUDE_STB_TILEMAP_EDITOR_H
#define STB_TILEMAP_INCLUDE_STB_TILEMAP_EDITOR_H

typedef struct stbte_tilemap stbte_tilemap;

// property types (kept for compilation even when STBTE_MAX_PROPERTIES == 0)
#define STBTE_PROP_none     0
#define STBTE_PROP_int      1
#define STBTE_PROP_float    2
#define STBTE_PROP_bool     3
#define STBTE_PROP_disabled 4

////////
//
// creation
//

extern stbte_tilemap *stbte_create_map(int map_x, int map_y, int map_layers, int spacing_x, int spacing_y, int max_tiles);
extern void stbte_define_tile(stbte_tilemap *tm, unsigned short id, unsigned int layermask, const char * category);

/////////
//
// map data access
//

#define STBTE_EMPTY    -1

extern void stbte_get_dimensions(stbte_tilemap *tm, int *max_x, int *max_y);
extern short* stbte_get_tile(stbte_tilemap *tm, int x, int y);
extern void stbte_set_dimensions(stbte_tilemap *tm, int max_x, int max_y);
extern void stbte_clear_map(stbte_tilemap *tm);
extern void stbte_set_tile(stbte_tilemap *tm, int x, int y, int layer, signed short tile);

////////
//
// optional
//

extern void stbte_set_background_tile(stbte_tilemap *tm, short id);

#endif

#ifdef STB_TILEMAP_EDITOR_IMPLEMENTATION

#ifndef STBTE_ASSERT
#define STBTE_ASSERT assert
#include <assert.h>
#endif

#ifdef _MSC_VER
#define STBTE__NOTUSED(v)  (void)(v)
#else
#define STBTE__NOTUSED(v)  (void)sizeof(v)
#endif

#ifndef STBTE_MAX_TILEMAP_X
#define STBTE_MAX_TILEMAP_X      200
#endif

#ifndef STBTE_MAX_TILEMAP_Y
#define STBTE_MAX_TILEMAP_Y      200
#endif

#ifndef STBTE_MAX_LAYERS
#define STBTE_MAX_LAYERS         8
#endif

#ifndef STBTE_MAX_CATEGORIES
#define STBTE_MAX_CATEGORIES     100
#endif

#ifndef STBTE_MAX_COPY
#define STBTE_MAX_COPY           65536
#endif

#ifndef STBTE_UNDO_BUFFER_BYTES
#define STBTE_UNDO_BUFFER_BYTES  (1 << 24) // 16 MB
#endif

#ifndef STBTE_PROP_TYPE
#define STBTE__NO_PROPS
#define STBTE_PROP_TYPE(n,td,tp)   0
#endif

#ifndef STBTE_PROP_NAME
#define STBTE_PROP_NAME(n,td,tp)  ""
#endif

#ifndef STBTE_MAX_PROPERTIES
#define STBTE_MAX_PROPERTIES           10
#endif

#ifndef STBTE_PROP_MIN
#define STBTE_PROP_MIN(n,td,tp)  0
#endif

#ifndef STBTE_PROP_MAX
#define STBTE_PROP_MAX(n,td,tp)  100.0
#endif

#define STBTE__UNDO_BUFFER_COUNT  (STBTE_UNDO_BUFFER_BYTES>>1)

#if STBTE_MAX_TILEMAP_X > 4096 || STBTE_MAX_TILEMAP_Y > 4096
#error "Maximum editable map size is 4096 x 4096"
#endif
#if STBTE_MAX_LAYERS > 32
#error "Maximum layers allowed is 32"
#endif
#if STBTE_UNDO_BUFFER_COUNT & (STBTE_UNDO_BUFFER_COUNT-1)
#error "Undo buffer size must be a power of 2"
#endif

#if STBTE_MAX_PROPERTIES == 0
#define STBTE__NO_PROPS
#endif

#ifdef STBTE__NO_PROPS
#undef STBTE_MAX_PROPERTIES
#define STBTE_MAX_PROPERTIES 1  // so we can declare arrays
#endif

typedef struct
{
   short x,y;
} stbte__link;

typedef struct
{
   short id;
   unsigned short category_id;
   char *category;
   unsigned int layermask;
} stbte__tileinfo;

#define MAX_LAYERMASK    (1 << (8*sizeof(unsigned int)))

typedef short stbte__tiledata;

#define STBTE__NO_TILE   -1

enum
{
   STBTE__tool_select,
   STBTE__tool_brush,
   STBTE__tool_erase,
   STBTE__tool_eyedrop,

   STBTE__num_tool,
};

enum
{
   STBTE__propmode_default,
   STBTE__propmode_always,
   STBTE__propmode_never,
};

enum
{
   STBTE__erase_none = -1,
   STBTE__erase_brushonly = 0,
   STBTE__erase_any = 1,
   STBTE__erase_all = 2,
};

typedef struct
{
   int tool;
   int initted;
   int brush_state; // used to decide which kind of erasing
   int eyedrop_x, eyedrop_y, eyedrop_last_layer;
   int undoing;
   int has_selection, select_x0, select_y0, select_x1, select_y1;
   short copybuffer[STBTE_MAX_COPY][STBTE_MAX_LAYERS];
   float copyprops[STBTE_MAX_COPY][STBTE_MAX_PROPERTIES];
#ifdef STBTE_ALLOW_LINK
   stbte__link copylinks[STBTE_MAX_COPY];
#endif
   int copy_src_x, copy_src_y;
   stbte_tilemap *copy_src;
   int copy_width,copy_height,has_copy,copy_has_props;
} stbte__ui_t;

// there's only one UI system at a time, so we can globalize this
static stbte__ui_t stbte__ui = { STBTE__tool_brush, 0 };

typedef struct
{
   const char *name;
   int locked;
   int hidden;
} stbte__layer;

enum
{
   STBTE__unlocked,
   STBTE__locked,
};

struct stbte_tilemap
{
    stbte__tiledata data[STBTE_MAX_TILEMAP_Y][STBTE_MAX_TILEMAP_X][STBTE_MAX_LAYERS];
    float props[STBTE_MAX_TILEMAP_Y][STBTE_MAX_TILEMAP_X][STBTE_MAX_PROPERTIES];
    #ifdef STBTE_ALLOW_LINK
    stbte__link link[STBTE_MAX_TILEMAP_Y][STBTE_MAX_TILEMAP_X];
    int linkcount[STBTE_MAX_TILEMAP_Y][STBTE_MAX_TILEMAP_X];
    #endif
    int max_x, max_y, num_layers;
    int cur_category, cur_tile, cur_layer;
    char *categories[STBTE_MAX_CATEGORIES];
    int num_categories;
    stbte__tileinfo *tiles;
    int num_tiles, max_tiles;
    unsigned char undo_available_valid;
    unsigned char undo_available;
    unsigned char redo_available;
    unsigned char padding;
    int cur_palette_count;
    int tileinfo_dirty;
    stbte__layer layerinfo[STBTE_MAX_LAYERS];
    int propmode;
    int solo_layer;
    int undo_pos, undo_len, redo_len;
    short background_tile;
    unsigned char id_in_use[32768>>3];
    short *undo_buffer;
};

static char *default_category = (char*) "[unassigned]";

static void stbte__init_gui(void)
{
   stbte__ui.initted = 1;
}

stbte_tilemap *stbte_create_map(int map_x, int map_y, int map_layers, int spacing_x, int spacing_y, int max_tiles)
{
   int i;
   stbte_tilemap *tm;
   STBTE_ASSERT(map_layers >= 0 && map_layers <= STBTE_MAX_LAYERS);
   STBTE_ASSERT(map_x >= 0 && map_x <= STBTE_MAX_TILEMAP_X);
   STBTE_ASSERT(map_y >= 0 && map_y <= STBTE_MAX_TILEMAP_Y);
   if (map_x < 0 || map_y < 0 || map_layers < 0 ||
       map_x > STBTE_MAX_TILEMAP_X || map_y > STBTE_MAX_TILEMAP_Y || map_layers > STBTE_MAX_LAYERS)
      return NULL;

   STBTE__NOTUSED(spacing_x);
   STBTE__NOTUSED(spacing_y);

   if (!stbte__ui.initted)
      stbte__init_gui();

   tm = (stbte_tilemap *) malloc(sizeof(*tm) + sizeof(*tm->tiles) * max_tiles + STBTE_UNDO_BUFFER_BYTES);
   if (tm == NULL)
      return NULL;

   tm->tiles = (stbte__tileinfo *) (tm+1);
   tm->undo_buffer = (short *) (tm->tiles + max_tiles);
   tm->num_layers = map_layers;
   tm->max_x = map_x;
   tm->max_y = map_y;
   tm->cur_category = -1;
   tm->cur_tile = 0;
   tm->solo_layer = -1;
   tm->undo_len = 0;
   tm->redo_len = 0;
   tm->undo_pos = 0;
   tm->propmode = 0;
   tm->cur_palette_count = 0;
   tm->undo_available_valid = 0;

   for (i=0; i < tm->num_layers; ++i) {
      tm->layerinfo[i].hidden = 0;
      tm->layerinfo[i].locked = STBTE__unlocked;
      tm->layerinfo[i].name   = 0;
   }

   tm->background_tile = STBTE__NO_TILE;
   stbte_clear_map(tm);

   tm->max_tiles = max_tiles;
   tm->num_tiles = 0;
   for (i=0; i < 32768/8; ++i)
      tm->id_in_use[i] = 0;
   tm->tileinfo_dirty = 1;
   return tm;
}

void stbte_set_background_tile(stbte_tilemap *tm, short id)
{
   int i;
   STBTE_ASSERT(id >= -1);
   if (id < -1)
      return;
   for (i=0; i < STBTE_MAX_TILEMAP_X * STBTE_MAX_TILEMAP_Y; ++i)
      if (tm->data[0][i][0] == -1)
         tm->data[0][i][0] = id;
   tm->background_tile = id;
}

void stbte_define_tile(stbte_tilemap *tm, unsigned short id, unsigned int layermask, const char * category_c)
{
   char *category = (char *) category_c;
   STBTE_ASSERT(id < 32768);
   STBTE_ASSERT(tm->num_tiles < tm->max_tiles);
   STBTE_ASSERT((tm->id_in_use[id>>3]&(1<<(id&7))) == 0);
   if (id >= 32768 || tm->num_tiles >= tm->max_tiles || (tm->id_in_use[id>>3]&(1<<(id&7))))
      return;

   if (category == NULL)
      category = (char*) default_category;
   tm->id_in_use[id>>3] |= 1 << (id&7);
   tm->tiles[tm->num_tiles].category    = category;
   tm->tiles[tm->num_tiles].id        = id;
   tm->tiles[tm->num_tiles].layermask = layermask;
   ++tm->num_tiles;
   tm->tileinfo_dirty = 1;
}

void stbte_get_dimensions(stbte_tilemap *tm, int *max_x, int *max_y)
{
   *max_x = tm->max_x;
   *max_y = tm->max_y;
}

short* stbte_get_tile(stbte_tilemap *tm, int x, int y)
{
   STBTE_ASSERT(x >= 0 && x < tm->max_x && y >= 0 && y < tm->max_y);
   if (x < 0 || x >= STBTE_MAX_TILEMAP_X || y < 0 || y >= STBTE_MAX_TILEMAP_Y)
      return NULL;
   return tm->data[y][x];
}

void stbte_set_dimensions(stbte_tilemap *tm, int map_x, int map_y)
{
   STBTE_ASSERT(map_x >= 0 && map_x <= STBTE_MAX_TILEMAP_X);
   STBTE_ASSERT(map_y >= 0 && map_y <= STBTE_MAX_TILEMAP_Y);
   if (map_x < 0 || map_y < 0 || map_x > STBTE_MAX_TILEMAP_X || map_y > STBTE_MAX_TILEMAP_Y)
      return;
   tm->max_x = map_x;
   tm->max_y = map_y;
}

void stbte_clear_map(stbte_tilemap *tm)
{
   int i,j;
   for (i=0; i < STBTE_MAX_TILEMAP_X * STBTE_MAX_TILEMAP_Y; ++i) {
      tm->data[0][i][0] = tm->background_tile;
      for (j=1; j < tm->num_layers; ++j)
         tm->data[0][i][j] = STBTE__NO_TILE;
      for (j=0; j < STBTE_MAX_PROPERTIES; ++j)
         tm->props[0][i][j] = 0;
      #ifdef STBTE_ALLOW_LINK
      tm->link[0][i].x = -1;
      tm->link[0][i].y = -1;
      tm->linkcount[0][i] = 0;
      #endif
   }
}

void stbte_set_tile(stbte_tilemap *tm, int x, int y, int layer, signed short tile)
{
   STBTE_ASSERT(x >= 0 && x < tm->max_x && y >= 0 && y < tm->max_y);
   STBTE_ASSERT(layer >= 0 && layer < tm->num_layers);
   STBTE_ASSERT(tile >= -1);
   if (x < 0 || x >= STBTE_MAX_TILEMAP_X || y < 0 || y >= STBTE_MAX_TILEMAP_Y)
      return;
   if (layer < 0 || layer >= tm->num_layers || tile < -1)
      return;
   tm->data[y][x][layer] = tile;
}

static void stbte__choose_category(stbte_tilemap *tm, int category)
{
   int i,n=0;
   tm->cur_category = category;
   for (i=0; i < tm->num_tiles; ++i)
      if (tm->tiles[i].category_id == category || category == -1)
         ++n;
   tm->cur_palette_count = n;
}

static int stbte__strequal(char *p, char *q)
{
   while (*p)
      if (*p++ != *q++) return 0;
   return *q == 0;
}

static void stbte__compute_tileinfo(stbte_tilemap *tm)
{
   int i,j;

   tm->num_categories=0;

   for (i=0; i < tm->num_tiles; ++i) {
      stbte__tileinfo *t = &tm->tiles[i];
      // find category
      for (j=0; j < tm->num_categories; ++j)
         if (stbte__strequal(t->category, tm->categories[j]))
            goto found;
      tm->categories[j] = t->category;
      ++tm->num_categories;
     found:
      t->category_id = (unsigned short) j;
   }

   if (tm->cur_category > tm->num_categories) {
      tm->cur_category = -1;
   }

   stbte__choose_category(tm, tm->cur_category);

   tm->tileinfo_dirty = 0;
}

static void stbte__prepare_tileinfo(stbte_tilemap *tm)
{
   if (tm->tileinfo_dirty)
      stbte__compute_tileinfo(tm);
}


/////////////////////// undo system ////////////////////////

#define stbte__wrap(pos)            ((pos) & (STBTE__UNDO_BUFFER_COUNT-1))

#define STBTE__undo_record  -2
#define STBTE__redo_record  -3
#define STBTE__undo_junk    -4

enum
{
   STBTE__undo_none,
   STBTE__undo_record_mode,
   STBTE__undo_block,
};

static void stbte__write_undo(stbte_tilemap *tm, short value)
{
   int pos = tm->undo_pos;
   tm->undo_buffer[pos] = value;
   tm->undo_pos = stbte__wrap(pos+1);
   tm->undo_len += (tm->undo_len < STBTE__UNDO_BUFFER_COUNT-2);
   tm->redo_len -= (tm->redo_len > 0);
   tm->undo_available_valid = 0;
}

static void stbte__write_redo(stbte_tilemap *tm, short value)
{
   int pos = tm->undo_pos;
   tm->undo_buffer[pos] = value;
   tm->undo_pos = stbte__wrap(pos-1);
   tm->redo_len += (tm->redo_len < STBTE__UNDO_BUFFER_COUNT-2);
   tm->undo_len -= (tm->undo_len > 0);
   tm->undo_available_valid = 0;
}

static void stbte__begin_undo(stbte_tilemap *tm)
{
   tm->redo_len = 0;
   stbte__write_undo(tm, STBTE__undo_record);
   stbte__ui.undoing = 1;
}

static void stbte__end_undo(stbte_tilemap *tm)
{
   if (stbte__ui.undoing) {
      // check if anything got written
      int pos = stbte__wrap(tm->undo_pos-1);
      if (tm->undo_buffer[pos] == STBTE__undo_record) {
         // empty undo record, move back
         tm->undo_pos = pos;
         STBTE_ASSERT(tm->undo_len > 0);
         tm->undo_len -= 1;
      }
      tm->undo_buffer[tm->undo_pos] = STBTE__undo_junk;
      stbte__ui.undoing = 0;
   }
}

static void stbte__undo_record(stbte_tilemap *tm, int x, int y, int i, int v)
{
   STBTE_ASSERT(stbte__ui.undoing);
   if (stbte__ui.undoing) {
      stbte__write_undo(tm, v);
      stbte__write_undo(tm, x);
      stbte__write_undo(tm, y);
      stbte__write_undo(tm, i);
   }
}

static void stbte__redo_record(stbte_tilemap *tm, int x, int y, int i, int v)
{
   stbte__write_redo(tm, v);
   stbte__write_redo(tm, x);
   stbte__write_redo(tm, y);
   stbte__write_redo(tm, i);
}

static float stbte__extract_float(short s0, short s1)
{
   union { float f; short s[2]; } converter;
   converter.s[0] = s0;
   converter.s[1] = s1;
   return converter.f;
}

static short stbte__extract_short(float f, int slot)
{
   union { float f; short s[2]; } converter;
   converter.f = f;
   return converter.s[slot];
}

static void stbte__undo_record_prop(stbte_tilemap *tm, int x, int y, int i, short s0, short s1)
{
   STBTE_ASSERT(stbte__ui.undoing);
   if (stbte__ui.undoing) {
      stbte__write_undo(tm, s1);
      stbte__write_undo(tm, s0);
      stbte__write_undo(tm, x);
      stbte__write_undo(tm, y);
      stbte__write_undo(tm, 256+i);
   }
}

static void stbte__undo_record_prop_float(stbte_tilemap *tm, int x, int y, int i, float f)
{
   stbte__undo_record_prop(tm, x,y,i, stbte__extract_short(f,0), stbte__extract_short(f,1));
}

static void stbte__redo_record_prop(stbte_tilemap *tm, int x, int y, int i, short s0, short s1)
{
   stbte__write_redo(tm, s1);
   stbte__write_redo(tm, s0);
   stbte__write_redo(tm, x);
   stbte__write_redo(tm, y);
   stbte__write_redo(tm, 256+i);
}


static int stbte__undo_find_end(stbte_tilemap *tm)
{
   // first scan through for the end record
   int i, pos = stbte__wrap(tm->undo_pos-1);
   for (i=0; i < tm->undo_len;) {
      STBTE_ASSERT(tm->undo_buffer[pos] != STBTE__undo_junk);
      if (tm->undo_buffer[pos] == STBTE__undo_record)
         break;
      if (tm->undo_buffer[pos] >= 255)
         pos = stbte__wrap(pos-5), i += 5;
      else
         pos = stbte__wrap(pos-4), i += 4;
   }
   if (i >= tm->undo_len)
      return -1;
   return pos;
}

#ifdef STBTE_ALLOW_LINK
static void stbte__set_link(stbte_tilemap *tm, int src_x, int src_y, int dest_x, int dest_y, int undo_mode);
#endif

static void stbte__undo(stbte_tilemap *tm)
{
   int i, pos, endpos;
   endpos = stbte__undo_find_end(tm);
   if (endpos < 0)
      return;

   // we found a complete undo record
   pos = stbte__wrap(tm->undo_pos-1);

   // start a redo record
   stbte__write_redo(tm, STBTE__redo_record);

   // so now go back through undo and apply in reverse
   // order, and copy it to redo
   for (i=0; endpos != pos; i += 4) {
      int x,y,n,v;
      // get the undo entry
      n = tm->undo_buffer[pos];
      y = tm->undo_buffer[stbte__wrap(pos-1)];
      x = tm->undo_buffer[stbte__wrap(pos-2)];
      v = tm->undo_buffer[stbte__wrap(pos-3)];
      if (n >= 255) {
         short s0=0,s1=0;
         int v2 = tm->undo_buffer[stbte__wrap(pos-4)];
         pos = stbte__wrap(pos-5);
         if (n > 255) {
            float vf = stbte__extract_float(v, v2);
            s0 = stbte__extract_short(tm->props[y][x][n-256], 0);
            s1 = stbte__extract_short(tm->props[y][x][n-256], 1);
            tm->props[y][x][n-256] = vf;
         } else {
#ifdef STBTE_ALLOW_LINK
            s0 = tm->link[y][x].x;
            s1 = tm->link[y][x].y;
            stbte__set_link(tm, x,y, v, v2, STBTE__undo_none);
#endif
         }
         // write the redo entry
         stbte__redo_record_prop(tm, x, y, n-256, s0,s1);
      } else {
         pos = stbte__wrap(pos-4);
         // write the redo entry
         stbte__redo_record(tm, x, y, n, tm->data[y][x][n]);
         // apply the undo entry
         tm->data[y][x][n] = (short) v;
      }
   }
   // overwrite undo record with junk
   tm->undo_buffer[tm->undo_pos] = STBTE__undo_junk;
}

static int stbte__redo_find_end(stbte_tilemap *tm)
{
   // first scan through for the end record
   int i, pos = stbte__wrap(tm->undo_pos+1);
   for (i=0; i < tm->redo_len;) {
      STBTE_ASSERT(tm->undo_buffer[pos] != STBTE__undo_junk);
      if (tm->undo_buffer[pos] == STBTE__redo_record)
         break;
      if (tm->undo_buffer[pos] >= 255)
         pos = stbte__wrap(pos+5), i += 5;
      else
         pos = stbte__wrap(pos+4), i += 4;
   }
   if (i >= tm->redo_len)
      return -1;
   return pos;
}

static void stbte__redo(stbte_tilemap *tm)
{
   int i, pos, endpos;
   endpos = stbte__redo_find_end(tm);
   if (endpos < 0)
      return;

   pos = stbte__wrap(tm->undo_pos+1);

   // start an undo record
   stbte__write_undo(tm, STBTE__undo_record);

   for (i=0; pos != endpos; i += 4) {
      int x,y,n,v;
      n = tm->undo_buffer[pos];
      y = tm->undo_buffer[stbte__wrap(pos+1)];
      x = tm->undo_buffer[stbte__wrap(pos+2)];
      v = tm->undo_buffer[stbte__wrap(pos+3)];
      if (n >= 255) {
         int v2 = tm->undo_buffer[stbte__wrap(pos+4)];
         short s0=0,s1=0;
         pos = stbte__wrap(pos+5);
         if (n > 255) {
            float vf = stbte__extract_float(v, v2);
            s0 = stbte__extract_short(tm->props[y][x][n-256],0);
            s1 = stbte__extract_short(tm->props[y][x][n-256],1);
            tm->props[y][x][n-256] = vf;
         } else {
#ifdef STBTE_ALLOW_LINK
            s0 = tm->link[y][x].x;
            s1 = tm->link[y][x].y;
            stbte__set_link(tm, x,y,v,v2, STBTE__undo_none);
#endif
         }
         stbte__write_undo(tm, s1);
         stbte__write_undo(tm, s0);
         stbte__write_undo(tm, x);
         stbte__write_undo(tm, y);
         stbte__write_undo(tm, n);
      } else {
         pos = stbte__wrap(pos+4);
         stbte__write_undo(tm, tm->data[y][x][n]);
         stbte__write_undo(tm, x);
         stbte__write_undo(tm, y);
         stbte__write_undo(tm, n);
         tm->data[y][x][n] = (short) v;
      }
   }
   tm->undo_buffer[tm->undo_pos] = STBTE__undo_junk;
}

static void stbte__recompute_undo_available(stbte_tilemap *tm)
{
   tm->undo_available = (stbte__undo_find_end(tm) >= 0);
   tm->redo_available = (stbte__redo_find_end(tm) >= 0);
}

#ifdef STBTE_ALLOW_LINK
static void stbte__set_link(stbte_tilemap *tm, int src_x, int src_y, int dest_x, int dest_y, int undo_mode)
{
   stbte__link *a;
   STBTE_ASSERT(src_x >= 0 && src_x < STBTE_MAX_TILEMAP_X && src_y >= 0 && src_y < STBTE_MAX_TILEMAP_Y);
   a = &tm->link[src_y][src_x];
   if (a->x == dest_x && a->y == dest_y)
      return;
   if (undo_mode != STBTE__undo_none) {
      if (undo_mode == STBTE__undo_block) stbte__begin_undo(tm);
      stbte__undo_record_prop(tm, src_x, src_y, -1, a->x, a->y);
      if (undo_mode == STBTE__undo_block) stbte__end_undo(tm);
   }
   if (a->x >= 0) {
      STBTE_ASSERT(tm->linkcount[a->y][a->x] > 0);
      --tm->linkcount[a->y][a->x];
   }
   if (dest_x >= 0) {
      ++tm->linkcount[dest_y][dest_x];
   }
   a->x = dest_x;
   a->y = dest_y;
}
#endif


/////////////////////// editor operations ////////////////////////

static void stbte__alert(const char *msg)
{
   (void)msg;
}

#define STBTE__BG(tm,layer) ((layer) == 0 ? (tm)->background_tile : STBTE__NO_TILE)

static void stbte__brush(stbte_tilemap *tm, int x, int y)
{
   stbte__tileinfo *ti;
   int i;

   if (tm->cur_tile < 0) return;

   ti = &tm->tiles[tm->cur_tile];

   for (i=0; i < tm->num_layers; ++i) {
      if (!(ti->layermask & (1 << i)))
         continue;

      if (i != tm->solo_layer) {
         if (tm->cur_layer >= 0 && i != tm->cur_layer)
            continue;
         if (tm->layerinfo[i].hidden)
            continue;
         if (tm->layerinfo[i].locked)
            continue;
      }

      stbte__undo_record(tm,x,y,i,tm->data[y][x][i]);
      tm->data[y][x][i] = ti->id;
      return;
   }
}

static int stbte__erase(stbte_tilemap *tm, int x, int y, int allow_any)
{
   stbte__tileinfo *ti = tm->cur_tile >= 0 ? &tm->tiles[tm->cur_tile] : NULL;
   int i;

   if (allow_any == STBTE__erase_none)
      return allow_any;

   i = tm->cur_layer;
   if (tm->solo_layer >= 0)
      i = tm->solo_layer;

   if (i >= 0) {
      short bg = (i == 0 ? tm->background_tile : -1);
      if (tm->solo_layer < 0) {
         if (tm->layerinfo[i].hidden) return STBTE__erase_none;
         if (tm->layerinfo[i].locked) return STBTE__erase_none;
      }
      if (tm->data[y][x][i] == bg)
         return STBTE__erase_none;
      if (ti && tm->data[y][x][i] == ti->id && (i != 0 || ti->id != tm->background_tile)) {
         stbte__undo_record(tm,x,y,i,tm->data[y][x][i]);
         tm->data[y][x][i] = bg;
         return STBTE__erase_brushonly;
      }
      if (allow_any == STBTE__erase_any) {
         stbte__undo_record(tm,x,y,i,tm->data[y][x][i]);
         tm->data[y][x][i] = bg;
         return STBTE__erase_any;
      }
      return STBTE__erase_none;
   }

   if (ti && allow_any != STBTE__erase_all) {
      for (i=tm->num_layers-1; i >= 0; --i) {
         if (tm->data[y][x][i] != ti->id)
            continue;
         if (tm->layerinfo[i].locked || tm->layerinfo[i].hidden)
            continue;
         if (i == 0 && tm->data[y][x][i] == tm->background_tile)
            return STBTE__erase_none;
         stbte__undo_record(tm,x,y,i,tm->data[y][x][i]);
         tm->data[y][x][i] = STBTE__BG(tm,i);
         return STBTE__erase_brushonly;
      }
   }

   if (allow_any != STBTE__erase_any && allow_any != STBTE__erase_all)
      return STBTE__erase_none;

   for (i=tm->num_layers-1; i >= 0; --i) {
      if (tm->data[y][x][i] < 0)
         continue;
      if (tm->layerinfo[i].locked || tm->layerinfo[i].hidden)
         continue;
      if (i == 0 && tm->data[y][x][i] == tm->background_tile)
         return STBTE__erase_none;
      stbte__undo_record(tm,x,y,i,tm->data[y][x][i]);
      tm->data[y][x][i] = STBTE__BG(tm,i);
      if (allow_any != STBTE__erase_all)
         return STBTE__erase_any;
   }
   if (allow_any == STBTE__erase_all)
      return allow_any;
   return STBTE__erase_none;
}

static int stbte__find_tile(stbte_tilemap *tm, int tile_id)
{
   int i;
   for (i=0; i < tm->num_tiles; ++i)
      if (tm->tiles[i].id == tile_id)
         return i;
   stbte__alert("Eyedropped tile that isn't in tileset");
   return -1;
}

static void stbte__eyedrop(stbte_tilemap *tm, int x, int y)
{
   int i,j;

   if (stbte__ui.eyedrop_x != x || stbte__ui.eyedrop_y != y) {
      stbte__ui.eyedrop_x = x;
      stbte__ui.eyedrop_y = y;
      stbte__ui.eyedrop_last_layer = tm->num_layers;
   }

   i = tm->cur_layer;
   if (tm->solo_layer >= 0)
      i = tm->solo_layer;
   if (i >= 0) {
      if (tm->data[y][x][i] == STBTE__NO_TILE)
         return;
      tm->cur_tile = stbte__find_tile(tm, tm->data[y][x][i]);
      return;
   }

   i = stbte__ui.eyedrop_last_layer;
   for (j=0; j < tm->num_layers; ++j) {
      if (--i < 0)
         i = tm->num_layers-1;
      if (tm->layerinfo[i].hidden)
         continue;
      if (tm->data[y][x][i] == STBTE__NO_TILE)
         continue;
      stbte__ui.eyedrop_last_layer = i;
      tm->cur_tile = stbte__find_tile(tm, tm->data[y][x][i]);
      return;
   }
}

static int stbte__should_copy_properties(stbte_tilemap *tm)
{
   int i;
   if (tm->propmode == STBTE__propmode_always)
      return 1;
   if (tm->propmode == STBTE__propmode_never)
      return 0;
   if (tm->solo_layer >= 0 || tm->cur_layer >= 0)
      return 0;
   for (i=0; i < tm->num_layers; ++i)
      if (tm->layerinfo[i].hidden || tm->layerinfo[i].locked)
         return 0;
   return 1;
}

static void stbte__paste_stack(stbte_tilemap *tm, short result[], short dest[], short src[], int dragging)
{
   int i;
   STBTE__NOTUSED(dragging);

   i = tm->cur_layer;
   if (tm->solo_layer >= 0)
      i = tm->solo_layer;
   if (i >= 0) {
      if (tm->solo_layer < 0) {
         if (tm->layerinfo[i].hidden) return;
         if (tm->layerinfo[i].locked) return;
      }
      result[i] = dest[i];
      if (src[i] != STBTE__BG(tm,i))
         result[i] = src[i];
      return;
   }

   for (i=0; i < tm->num_layers; ++i) {
      result[i] = dest[i];
      if (src[i] != STBTE__NO_TILE)
         if (!tm->layerinfo[i].hidden && !tm->layerinfo[i].locked)
               result[i] = src[i];
   }
}

static void stbte__fillrect(stbte_tilemap *tm, int x0, int y0, int x1, int y1, int fill)
{
   int i,j;

   stbte__begin_undo(tm);
   if (x0 > x1) i=x0,x0=x1,x1=i;
   if (y0 > y1) j=y0,y0=y1,y1=j;
   for (j=y0; j <= y1; ++j)
      for (i=x0; i <= x1; ++i)
         if (fill)
            stbte__brush(tm, i,j);
         else
            stbte__erase(tm, i,j,STBTE__erase_any);
   stbte__end_undo(tm);
}

static void stbte__select_rect(stbte_tilemap *tm, int x0, int y0, int x1, int y1)
{
   STBTE__NOTUSED(tm);
   stbte__ui.has_selection = 1;
   stbte__ui.select_x0 = (x0 < x1 ? x0 : x1);
   stbte__ui.select_x1 = (x0 < x1 ? x1 : x0);
   stbte__ui.select_y0 = (y0 < y1 ? y0 : y1);
   stbte__ui.select_y1 = (y0 < y1 ? y1 : y0);
}

static void stbte__copy_properties(float *dest, float *src)
{
   int i;
   for (i=0; i < STBTE_MAX_PROPERTIES; ++i)
      dest[i] = src[i];
}

static void stbte__copy_cut(stbte_tilemap *tm, int cut)
{
   int i,j,n,w,h,p=0;
   int copy_props = stbte__should_copy_properties(tm);
   if (!stbte__ui.has_selection)
      return;
   w = stbte__ui.select_x1 - stbte__ui.select_x0 + 1;
   h = stbte__ui.select_y1 - stbte__ui.select_y0 + 1;
   if (STBTE_MAX_COPY / w < h) {
      stbte__alert("Selection too large for copy buffer, increase STBTE_MAX_COPY");
      return;
   }

   for (i=0; i < w*h; ++i)
      for (n=0; n < tm->num_layers; ++n)
         stbte__ui.copybuffer[i][n] = STBTE__NO_TILE;

   if (cut)
      stbte__begin_undo(tm);
   for (j=stbte__ui.select_y0; j <= stbte__ui.select_y1; ++j) {
      for (i=stbte__ui.select_x0; i <= stbte__ui.select_x1; ++i) {
         for (n=0; n < tm->num_layers; ++n) {
            if (tm->solo_layer >= 0) {
               if (tm->solo_layer != n)
                  continue;
            } else {
               if (tm->cur_layer >= 0)
                  if (tm->cur_layer != n)
                     continue;
               if (tm->layerinfo[n].hidden)
                  continue;
               if (cut && tm->layerinfo[n].locked)
                  continue;
            }
            stbte__ui.copybuffer[p][n] = tm->data[j][i][n];
            if (cut) {
               stbte__undo_record(tm,i,j,n, tm->data[j][i][n]);
               tm->data[j][i][n] = (n==0 ? tm->background_tile : -1);
            }
         }
         if (copy_props) {
            stbte__copy_properties(stbte__ui.copyprops[p], tm->props[j][i]);
#ifdef STBTE_ALLOW_LINK
            stbte__ui.copylinks[p] = tm->link[j][i];
            if (cut)
               stbte__set_link(tm, i,j,-1,-1, STBTE__undo_record_mode);
#endif
         }
         ++p;
      }
   }
   if (cut)
      stbte__end_undo(tm);
   stbte__ui.copy_width = w;
   stbte__ui.copy_height = h;
   stbte__ui.has_copy = 1;
   stbte__ui.copy_has_props = copy_props;
   stbte__ui.copy_src = tm;
   stbte__ui.copy_src_x = stbte__ui.select_x0;
   stbte__ui.copy_src_y = stbte__ui.select_y0;
}

static void stbte__paste(stbte_tilemap *tm, int mapx, int mapy)
{
   int w = stbte__ui.copy_width;
   int h = stbte__ui.copy_height;
   int i,j,k,p;
   int x = mapx - (w>>1);
   int y = mapy - (h>>1);
   int copy_props = stbte__should_copy_properties(tm) && stbte__ui.copy_has_props;
   if (stbte__ui.has_copy == 0)
      return;
   stbte__begin_undo(tm);
   p = 0;
   for (j=0; j < h; ++j) {
      for (i=0; i < w; ++i) {
         if (y+j >= 0 && y+j < tm->max_y && x+i >= 0 && x+i < tm->max_x) {
            short tilestack[STBTE_MAX_LAYERS];
            for (k=0; k < tm->num_layers; ++k)
               tilestack[k] = tm->data[y+j][x+i][k];
            stbte__paste_stack(tm, tilestack, tilestack, stbte__ui.copybuffer[p], 0);
            for (k=0; k < tm->num_layers; ++k) {
               if (tilestack[k] != tm->data[y+j][x+i][k]) {
                  stbte__undo_record(tm, x+i,y+j,k, tm->data[y+j][x+i][k]);
                  tm->data[y+j][x+i][k] = tilestack[k];
               }
            }
         }
         if (copy_props) {
#ifdef STBTE_ALLOW_LINK
            int destx = -1, desty = -1;
            stbte__link *link = &stbte__ui.copylinks[p];

            if (link->x >= 0 && link->y >= 0) {
               if (link->x >= stbte__ui.copy_src_x && link->x < stbte__ui.copy_src_x + w &&
                   link->y >= stbte__ui.copy_src_y && link->y < stbte__ui.copy_src_y + h) {
                  destx = x + (link->x - stbte__ui.copy_src_x);
                  desty = y + (link->y - stbte__ui.copy_src_y);
               } else if (tm == stbte__ui.copy_src) {
                  if (!(link->x >= x && link->x < x + w && link->y >= y && link->y < y + h)) {
                     destx = link->x;
                     desty = link->y;
                  }
               }
            }
            if (destx < 0 || destx >= tm->max_x || desty < 0 || desty >= tm->max_y)
               destx = -1, desty = -1;
            stbte__set_link(tm, x+i, y+j, destx, desty, STBTE__undo_record_mode);
#endif
            for (k=0; k < STBTE_MAX_PROPERTIES; ++k) {
               if (tm->props[y+j][x+i][k] != stbte__ui.copyprops[p][k])
                  stbte__undo_record_prop_float(tm, x+i, y+j, k, tm->props[y+j][x+i][k]);
            }
            stbte__copy_properties(tm->props[y+j][x+i], stbte__ui.copyprops[p]);
         }
         ++p;
      }
   }
   stbte__end_undo(tm);
}


#endif // STB_TILEMAP_EDITOR_IMPLEMENTATION

/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
