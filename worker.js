/**
 * stb_tilemap_editor Dedicated Worker
 *
 * Loads the stb_tilemap_editor WASM module, creates SharedArrayBuffer-backed
 * memory, and runs the editor frame loop. The main thread reads draw commands
 * and writes input events through the shared WASM linear memory.
 *
 * Communication protocol:
 *   Main -> Worker: postMessage({ type, ...payload })
 *   Worker -> Main: postMessage({ type, ...data })
 *
 * Message types:
 *   'init'    -> Load WASM, create shared memory, return buffer info
 *   'resize'  -> Update editor display dimensions
 *   'start'   -> Start the frame loop
 *   'stop'    -> Stop the frame loop
 */

/** @type {WebAssembly.Instance} */
let wasmInstance = null

/** @type {WebAssembly.Memory} */
let wasmMemory = null

/** @type {number|null} */
let frameLoopId = null

/** @type {number} Pointer offsets into WASM memory */
let cmdBufAPtr = 0
let cmdBufBPtr = 0
let evtBufPtr = 0
let controlBlockPtr = 0
let evtHeadPtr = 0
let cmdBufSize = 0
let evtBufSize = 0
let controlBlockSize = 0

/**
 * Initialize the WASM module with shared memory.
 * @param {string} wasmUrl - URL to fetch the WASM binary
 * @param {Object} config - Editor configuration
 * @param {number} config.mapWidth - Tilemap width in tiles
 * @param {number} config.mapHeight - Tilemap height in tiles
 * @param {number} config.numLayers - Number of tile layers
 * @param {number} config.displayWidth - Initial display width in pixels
 * @param {number} config.displayHeight - Initial display height in pixels
 */
async function handleInit(wasmUrl, config) {
  try {
    // Fetch the WASM binary
    const response = await fetch(wasmUrl)
    if (!response.ok) {
      throw new Error(`Failed to fetch WASM: ${response.status} ${response.statusText}`)
    }
    const wasmBytes = await response.arrayBuffer()

    // Create shared memory.
    // The WASM module was compiled with 158 initial pages (~10MB).
    // We create a shared memory with the same initial size so the main
    // thread can directly read the command buffers via Atomics.
    const initialPages = 158
    const maxPages = 512 // allow growing up to 32MB

    wasmMemory = new WebAssembly.Memory({
      initial: initialPages,
      maximum: maxPages,
      shared: true
    })

    // Instantiate with the shared memory
    const importObject = {
      env: {
        memory: wasmMemory
      }
    }

    const { instance } = await WebAssembly.instantiate(wasmBytes, importObject)
    wasmInstance = instance

    // Get buffer pointers from WASM exports
    cmdBufAPtr = instance.exports.get_cmd_buf_a_ptr()
    cmdBufBPtr = instance.exports.get_cmd_buf_b_ptr()
    evtBufPtr = instance.exports.get_event_buf_ptr()
    controlBlockPtr = instance.exports.get_control_block_ptr()
    evtHeadPtr = instance.exports.get_evt_head_ptr()
    cmdBufSize = instance.exports.get_cmd_buf_size()
    evtBufSize = instance.exports.get_evt_buf_size()
    controlBlockSize = instance.exports.get_control_block_size()

    // Write initial config to control block before init()
    const controlView = new Int32Array(wasmMemory.buffer, controlBlockPtr, controlBlockSize / 4)
    // Control block layout (uint32 offsets):
    //  0: active_read_buf
    //  1: cmd_count_a
    //  2: cmd_count_b
    //  3: evt_head
    //  4: evt_tail
    //  5: frame_ready
    //  6: editor_width
    //  7: editor_height
    //  8: initialized
    //  9: map_width
    // 10: map_height
    // 11: num_layers
    Atomics.store(controlView, 9, config.mapWidth || 16)
    Atomics.store(controlView, 10, config.mapHeight || 16)
    Atomics.store(controlView, 11, config.numLayers || 2)

    // Call init()
    const initResult = instance.exports.init()
    if (initResult !== 0) {
      throw new Error(`WASM init() failed with code ${initResult}`)
    }

    // Set initial display size
    if (config.displayWidth && config.displayHeight) {
      // Write resize event directly to the event buffer
      const evtView = new Int32Array(wasmMemory.buffer, evtBufPtr, evtBufSize)
      evtView[0] = 5 // EVT_RESIZE
      evtView[1] = 0 // x0
      evtView[2] = 0 // y0
      evtView[3] = config.displayWidth
      evtView[4] = config.displayHeight
      // Update event head
      Atomics.store(controlView, 3, 5) // evt_head = 5 words
    }

    // Run one frame so there's something to display
    instance.exports.frame()

    // Send memory info back to main thread
    self.postMessage({
      type: 'init-success',
      memory: wasmMemory,
      pointers: {
        cmdBufA: cmdBufAPtr,
        cmdBufB: cmdBufBPtr,
        evtBuf: evtBufPtr,
        controlBlock: controlBlockPtr,
        evtHead: evtHeadPtr,
        cmdBufSize,
        evtBufSize,
        controlBlockSize
      }
    })
  } catch (err) {
    self.postMessage({
      type: 'error',
      error: { message: err.message, stack: err.stack }
    })
  }
}

/**
 * Run one editor frame: processes events, ticks, draws.
 */
function runFrame() {
  if (!wasmInstance) return

  try {
    wasmInstance.exports.frame()
  } catch (err) {
    console.error('stb-editor frame error:', err)
    stopFrameLoop()
  }
}

/**
 * Start the frame loop (~60fps via setInterval).
 * Using setInterval instead of requestAnimationFrame since we're in a worker.
 */
function startFrameLoop() {
  if (frameLoopId !== null) return
  frameLoopId = setInterval(runFrame, 16) // ~60fps
}

/**
 * Stop the frame loop.
 */
function stopFrameLoop() {
  if (frameLoopId !== null) {
    clearInterval(frameLoopId)
    frameLoopId = null
  }
}

// Message handler
self.onmessage = async (e) => {
  const { type, ...payload } = e.data

  switch (type) {
    case 'init':
      await handleInit(payload.wasmUrl, payload.config || {})
      break

    case 'start':
      startFrameLoop()
      self.postMessage({ type: 'started' })
      break

    case 'stop':
      stopFrameLoop()
      self.postMessage({ type: 'stopped' })
      break

    default:
      console.warn('stb-editor worker: unknown message type:', type)
  }
}
