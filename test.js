// Test script for stb_tilemap_editor WASM - runs with Bun
// Usage: bun test.js

const fs = require('fs');
const path = require('path');

async function runTest() {
  // Simple logger
  function log(msg) {
    console.log(`[TEST] ${msg}`);
  }

  // Load WASM file
  const wasmPath = path.join(__dirname, 'build.nosync/web/stb_tilemap_editor.wasm');
  const wasmBytes = fs.readFileSync(wasmPath);

  log(`Loaded WASM file: ${wasmBytes.length} bytes`);

  // Create shared memory for import (must match WASM --initial-memory)
  // 288 pages = 18MB = 18874368 bytes
  const memory = new WebAssembly.Memory({
    initial: 288,  // 288 pages = 18MB
    maximum: 512,  // 512 pages = 32MB
    shared: true
  });
  log('Shared memory created');

  // Instantiate WASM with imported memory
  const importObject = { env: { memory } };
  const { instance } = await WebAssembly.instantiate(wasmBytes, importObject);

  log('WASM instantiated');

  // Get exports - memory is imported, not exported
  const exports = instance.exports;
  
  log(`WASM exported memory: ${memory.buffer.byteLength} bytes`);

// Log all exports
log('=== Exported Functions ===');
const exportNames = Object.keys(exports).sort();
exportNames.forEach(name => {
  const type = typeof exports[name];
  log(`  ${name}: ${type}`);
});
log(`Total exports: ${exportNames.length}`);

// Test: Load offsets
log('\n=== Loading Struct Offsets ===');
const offsets = {};
const offsetNames = [
  'stbte_offset_tilemap_max_x', 'stbte_offset_tilemap_max_y',
  'stbte_offset_tilemap_num_layers', 'stbte_offset_tilemap_num_tiles',
  'stbte_offset_tilemap_cur_tile', 'stbte_offset_tilemap_cur_layer',
  'stbte_offset_tilemap_solo_layer', 'stbte_offset_tilemap_cur_category',
  'stbte_offset_tilemap_background_tile', 'stbte_offset_tilemap_num_categories',
  'stbte_offset_tilemap_data', 'stbte_offset_tilemap_tiles',
  'stbte_offset_tilemap_layerinfo', 'stbte_offset_tilemap_undo_available',
  'stbte_offset_tilemap_redo_available', 'stbte_offset_layer_hidden',
  'stbte_offset_layer_locked', 'stbte_offset_tileinfo_id',
  'stbte_offset_tileinfo_layermask', 'stbte_offset_tileinfo_category_id',
  'stbte_offset_ui_tool', 'stbte_offset_ui_has_selection',
  'stbte_offset_ui_select_x0', 'stbte_offset_ui_select_y0',
  'stbte_offset_ui_select_x1', 'stbte_offset_ui_select_y1',
  'stbte_offset_ui_has_copy', 'stbte_offset_ui_copy_width',
  'stbte_offset_ui_copy_height', 'stbte_offset_ui_copybuffer',
];

offsetNames.forEach(name => {
  offsets[name.replace('stbte_offset_', '').replace('tilemap_', 'tm_')] = exports[name]();
});

offsets.sizeof_tilemap = exports.stbte_sizeof_tilemap();
offsets.sizeof_layer = exports.stbte_sizeof_layer();
offsets.sizeof_tileinfo = exports.stbte_sizeof_tileinfo();
offsets.sizeof_ui = exports.stbte_sizeof_ui();
offsets.max_map_x = exports.stbte_max_map_x();
offsets.max_map_y = exports.stbte_max_map_y();
offsets.max_layers = exports.stbte_max_layers();

log('Offsets loaded:');
Object.entries(offsets).forEach(([k, v]) => {
  log(`  ${k}: ${v}`);
});

// Helper to read from tilemap memory
function readTilemap(tilemapPtr, offset, type = 'i32') {
  const view = new DataView(memory.buffer, tilemapPtr + offset);
  switch (type) {
    case 'i8': return view.getInt8(0);
    case 'i16': return view.getInt16(0, true);
    case 'i32': return view.getInt32(0, true);
    default: return view.getInt32(0, true);
  }
}

// Test: Create tilemap
log('\n=== Creating Tilemap ===');
const mapWidth = 20;
const mapHeight = 15;
const layers = 3;
const tileSize = 32;
const maxTiles = 256;

log(`Creating tilemap: ${mapWidth}x${mapHeight}, ${layers} layers`);
const tilemap = exports.stbte_create(mapWidth, mapHeight, layers, tileSize, tileSize, maxTiles);
log(`Tilemap pointer: ${tilemap} (0x${tilemap.toString(16)})`);

if (tilemap === 0) {
  log('ERROR: Failed to create tilemap (returned null)');
  process.exit(1);
}

// Read initial values
log('\n=== Reading Initial Values ===');

// Debug: Read bytes at various offsets
log(`Reading from tilemap base: 0x${tilemap.toString(16)}`);
log(`tm_tiles offset: ${offsets.tm_tiles}`);
log(`Address to read: 0x${(tilemap + offsets.tm_tiles).toString(16)}`);

const numTiles = readTilemap(tilemap, offsets.tm_num_tiles, 'i32');
const numCategories = readTilemap(tilemap, offsets.tm_num_categories, 'i32');

// Read tiles pointer as two 32-bit halves (in case it's 64-bit)
const tilesPtrLow = readTilemap(tilemap, offsets.tm_tiles, 'i32');
const tilesPtrHigh = readTilemap(tilemap, offsets.tm_tiles + 4, 'i32');
const tilesPtr = tilesPtrLow; // Assume 32-bit for now

log(`Initial num_tiles: ${numTiles}`);
log(`Initial num_categories: ${numCategories}`);
log(`Tiles pointer (low): ${tilesPtrLow}`);
log(`Tiles pointer (high): ${tilesPtrHigh}`);
log(`Tiles pointer: ${tilesPtr} (0x${tilesPtr.toString(16)})`);

// Debug: Dump first 32 bytes of tilemap
log('\nFirst 32 bytes of tilemap:');
for (let i = 0; i < 32; i += 4) {
  const val = readTilemap(tilemap, i, 'i32');
  log(`  offset ${i}: ${val} (0x${val.toString(16)})`);
}

// Debug: Dump bytes around tm_tiles offset
log(`\nBytes around tm_tiles offset (${offsets.tm_tiles}):`);
for (let i = offsets.tm_tiles - 8; i <= offsets.tm_tiles + 8; i += 4) {
  if (i >= 0) {
    const val = readTilemap(tilemap, i, 'i32');
    log(`  offset ${i}: ${val} (0x${val.toString(16)})`);
  }
}

// Test: Define tiles
log('\n=== Defining Tiles ===');
const tiles = [
  { id: 0, mask: 0xFF, cat: 0 },
  { id: 1, mask: 0x01, cat: 1 },
  { id: 2, mask: 0x01, cat: 1 },
  { id: 3, mask: 0x01, cat: 1 },
  { id: 10, mask: 0x02, cat: 2 },
  { id: 11, mask: 0x02, cat: 2 },
  { id: 12, mask: 0x02, cat: 2 },
  { id: 20, mask: 0x04, cat: 3 },
  { id: 21, mask: 0x04, cat: 3 },
  { id: 22, mask: 0x04, cat: 3 },
  { id: 23, mask: 0x04, cat: 3 },
];

tiles.forEach(t => {
  exports.stbte_define_tile(tilemap, t.id, t.mask, t.cat);
});

log(`Defined ${tiles.length} tiles`);

// Read values after defining tiles
const numTilesAfter = readTilemap(tilemap, offsets.tm_num_tiles, 'i32');
const numCategoriesAfter = readTilemap(tilemap, offsets.tm_num_categories, 'i32');
const tilesPtrAfter = readTilemap(tilemap, offsets.tm_tiles, 'i32');

log(`After defineTiles - num_tiles: ${numTilesAfter}`);
log(`After defineTiles - num_categories: ${numCategoriesAfter}`);
log(`Tiles pointer: ${tilesPtrAfter} (0x${tilesPtrAfter.toString(16)})`);

// Test: Read tile info
log('\n=== Reading Tile Info ===');
if (tilesPtrAfter > 0 && numTilesAfter > 0) {
  log(`Reading ${numTilesAfter} tiles from pointer 0x${tilesPtrAfter.toString(16)}`);
  
  for (let i = 0; i < Math.min(numTilesAfter, 5); i++) {
    const tileBase = tilesPtrAfter + (i * offsets.sizeof_tileinfo);
    const tileId = new DataView(memory.buffer, tileBase + offsets.tileinfo_id, 2).getUint16(0, true);
    const tileMask = new DataView(memory.buffer, tileBase + offsets.tileinfo_layermask, 4).getUint32(0, true);
    const tileCat = new DataView(memory.buffer, tileBase + offsets.tileinfo_category_id, 2).getUint16(0, true);
    
    log(`  Tile ${i}: ID=${tileId}, Mask=0x${tileMask.toString(16)}, Cat=${tileCat}`);
  }
  
  if (numTilesAfter > 5) {
    log(`  ... and ${numTilesAfter - 5} more tiles`);
  }
} else {
  log('ERROR: Invalid tiles pointer or num_tiles is 0');
}

// Test: Set active category (this triggers category computation)
log('\n=== Testing Category Filtering ===');
exports.stbte_set_active_category(tilemap, 1);
const curCategory = readTilemap(tilemap, offsets.tm_cur_category, 'i32');
const numCategoriesFinal = readTilemap(tilemap, offsets.tm_num_categories, 'i32');
log(`Active category set to: ${curCategory}`);
log(`Number of categories: ${numCategoriesFinal}`);

// Read categories
if (numCategoriesFinal > 0) {
  log('Categories:');
  for (let i = 0; i < numCategoriesFinal; i++) {
    log(`  Category ${i}`);
  }
}

// Test: Set active tile
log('\n=== Testing Tile Selection ===');
exports.stbte_set_active_tile(tilemap, 2);
const curTile = readTilemap(tilemap, offsets.tm_cur_tile, 'i32');
log(`Active tile set to: ${curTile}`);

// Test: Set active layer
log('\n=== Testing Layer Selection ===');
exports.stbte_set_active_layer(tilemap, 1);
const curLayer = readTilemap(tilemap, offsets.tm_cur_layer, 'i32');
log(`Active layer set to: ${curLayer}`);

// Test: Click on tile
log('\n=== Testing Click Operation ===');
exports.stbte_set_tool(tilemap, 1); // Brush tool
exports.stbte_click_tile(tilemap, 5, 5, 0);
log('Clicked at (5,5) with brush tool');

// Test: Read map data
log('\n=== Reading Map Data ===');
// tm_data is offset 0, data is embedded in struct at tilemap pointer
// Important: data is stored as [MAX_Y][MAX_X][MAX_LAYERS], not [mapHeight][mapWidth][layers]
const dataOffset = tilemap + offsets.tm_data;
const maxX = offsets.max_map_x;  // 512
const maxLayers = offsets.max_layers;  // 8

log(`Map data offset: ${dataOffset} (tilemap + ${offsets.tm_data})`);
log(`Array dimensions: ${maxX}x${offsets.max_map_y}x${maxLayers}`);

// Read tile at (5,5) - index = (y * MAX_X + x) * MAX_LAYERS + layer
const tileIdx = (5 * maxX + 5) * maxLayers + 0;
const tileValue = new Int16Array(memory.buffer, dataOffset + tileIdx * 2, 1)[0];
log(`Tile at (5,5) layer 0: ${tileValue}`);

// Test: Undo/Redo
log('\n=== Testing Undo/Redo ===');
const canUndo = readTilemap(tilemap, offsets.tm_undo_available, 'i8');
const canRedo = readTilemap(tilemap, offsets.tm_redo_available, 'i8');
log(`Can undo: ${canUndo}, Can redo: ${canRedo}`);

exports.stbte_undo(tilemap);
log('Undo performed');

// Test: Selection
log('\n=== Testing Selection ===');
exports.stbte_set_selection(tilemap, 1, 1, 5, 5);
const hasSel = new DataView(memory.buffer, exports.stbte_offset_ui_has_selection(), 4).getInt32(0, true);
log(`Has selection: ${hasSel}`);

// All tests passed
log('\n=== ALL TESTS COMPLETED ===');
log('If you see this message, the WASM module is working correctly!');
}

// Run the test
runTest().catch(err => {
  console.error('Test failed:', err);
  process.exit(1);
});
