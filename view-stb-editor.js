/**
 * view-stb-editor.js
 *
 * Browser view component for the stb_tilemap_editor WASM plugin.
 * Runs the stb editor in a dedicated Web Worker with SharedArrayBuffer,
 * reads draw commands from shared WASM memory, and renders to Canvas2D.
 * Mouse/keyboard events are forwarded to the editor via a shared event ring buffer.
 *
 * Architecture:
 *   Main Thread (this)       SharedArrayBuffer (WASM memory)            Worker
 *   ┌─────────────┐             ┌──────────────────────┐             ┌──────────┐
 *   │ Canvas2D    │◄── reads ── │ Command Buffer A/B   │ ◄── writes ─│ stb WASM │
 *   │ rAF loop    │             │ Event Ring Buffer    │ ── reads ──►│ frame()  │
 *   │ mouse evts  │── writes ──►│ Control Block        │             │ tick()   │
 *   └─────────────┘             └──────────────────────┘             │ draw()   │
 *                                                                    └──────────┘
 */

// Command types (must match main.c)
const CMD_RECT = 1
const CMD_TILE = 2
const DEBU_TILE_NR = false

// Event types (must match main.c)
const EVT_MOUSE_MOVE = 1
const EVT_MOUSE_BUTTON = 2
const EVT_MOUSE_WHEEL = 3
const EVT_ACTION = 4
const EVT_RESIZE = 5

// Control block field offsets (uint32 indices)
const CB_ACTIVE_READ_BUF = 0
const CB_CMD_COUNT_A = 1
const CB_CMD_COUNT_B = 2
const CB_EVT_HEAD = 3
const CB_EVT_TAIL = 4
const CB_FRAME_READY = 5
const CB_EDITOR_WIDTH = 6
const CB_EDITOR_HEIGHT = 7
const CB_INITIALIZED = 8

// Tile colors for placeholder rendering (Phase 1)
const TILE_COLORS = [
  '#4a6741', '#6b8c42', '#8fbc3e', '#3d5c3a', '#557a3e', '#2e4a30',
  '#7a5c3a', '#5a4030', '#8b7355', '#6b5a4a', '#9c8060', '#4a3828',
  '#3a5a8c', '#4a7ab0', '#2a4a6c', '#5a8ab0', '#3a6a9c', '#2a3a5c',
]

export default class ViewStbEditor extends HTMLElement {
  static get viewMeta() {
    return { displayName: 'STB Tilemap Editor', category: 'Tiles' }
  }

  constructor() {
    super()

    /** @type {HTMLCanvasElement} */
    this.canvas = document.createElement('canvas')
    this.ctx = this.canvas.getContext('2d')
    this.ctx.imageSmoothingEnabled = false

    /** @type {Worker|null} */
    this.worker = null

    /** @type {WebAssembly.Memory|null} */
    this.wasmMemory = null

    /** @type {Object} Buffer pointers into WASM memory */
    this.pointers = null

    /** @type {boolean} */
    this.running = false

    /** @type {number|null} */
    this.animFrameId = null

    /** @type {number} */
    this._requestId = 0

    /** @type {Map<number, {resolve: Function, reject: Function}>} */
    this._pending = new Map()

    /** @type {Map<number, {image: HTMLImageElement, sx: number, sy: number, sw: number, sh: number}>} */
    this.tileImages = new Map()
    /** @type {number} */
    this.tileSpacingX = 16
    /** @type {number} */
    this.tileSpacingY = 16

    // Bind methods
    this._renderLoop = this._renderLoop.bind(this)
    this._onMouseMove = this._onMouseMove.bind(this)
    this._onMouseDown = this._onMouseDown.bind(this)
    this._onMouseUp = this._onMouseUp.bind(this)
    this._onWheel = this._onWheel.bind(this)
    this._onContextMenu = this._onContextMenu.bind(this)
    this._onKeyDown = this._onKeyDown.bind(this)

    // Resize observer
    this._resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        if (entry.target === this) {
          this._handleResize(this.clientWidth, this.clientHeight)
        }
      }
    })
  }

  /**
   * Send a message to the worker and return a promise for the response.
   * @param {string} type - Message type
   * @param {Object} payload - Message payload
   * @returns {Promise<Object>}
   */
  async _workerCall(type, payload = {}) {
    const id = ++this._requestId
    const promise = new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject })
    })
    this.worker.postMessage({ type, id, ...payload })
    return promise
  }

  connectedCallback() {
    // Layout
    this.style.display = 'block'
    this.style.position = 'relative'
    this.style.width = '100%'
    this.style.height = '100%'
    this.style.overflow = 'hidden'
    this.style.backgroundColor = '#1a1a2e'

    // Canvas fills container
    this.canvas.style.display = 'block'
    this.canvas.style.width = '100%'
    this.canvas.style.height = '100%'
    this.appendChild(this.canvas)

    // Focus support
    if (!this.hasAttribute('tabindex')) {
      this.setAttribute('tabindex', '0')
    }

    // Start observing size
    this._resizeObserver.observe(this)

    // Event listeners on canvas
    this.canvas.addEventListener('mousemove', this._onMouseMove)
    this.canvas.addEventListener('mousedown', this._onMouseDown)
    this.canvas.addEventListener('mouseup', this._onMouseUp)
    this.canvas.addEventListener('wheel', this._onWheel, { passive: false })
    this.canvas.addEventListener('contextmenu', this._onContextMenu)
    this.addEventListener('keydown', this._onKeyDown)

    // Initialize the worker
    this._initWorker()
  }

  disconnectedCallback() {
    this.running = false
    if (this.animFrameId !== null) {
      cancelAnimationFrame(this.animFrameId)
      this.animFrameId = null
    }

    this._resizeObserver.disconnect()

    this.canvas.removeEventListener('mousemove', this._onMouseMove)
    this.canvas.removeEventListener('mousedown', this._onMouseDown)
    this.canvas.removeEventListener('mouseup', this._onMouseUp)
    this.canvas.removeEventListener('wheel', this._onWheel)
    this.canvas.removeEventListener('contextmenu', this._onContextMenu)
    this.removeEventListener('keydown', this._onKeyDown)

    if (this.worker) {
      // Reject all pending promises
      for (const [id, pending] of this._pending) {
        pending.reject(new Error('Worker terminated'))
      }
      this._pending.clear()

      // Send stop (ignore response since we terminate immediately)
      this.worker.postMessage({ type: 'stop', id: 0 })
      this.worker.terminate()
      this.worker = null
    }

  }

  // ── Worker initialization ───────────────────────────────────────────

  async _initWorker() {
    try {
      this.worker = new Worker(
        new URL('./wasm-worker.js', import.meta.url),
        { type: 'module' }
      )

      this.worker.onmessage = (e) => this._onWorkerMessage(e)
      this.worker.onerror = (err) => {
        console.error('stb-editor worker error:', err)
      }

      // Get initial dimensions
      const w = this.clientWidth || 800
      const h = this.clientHeight || 600

      // 1. Initialize WASM module
      const initResult = await this._workerCall('init', {
        wasmUrl: new URL('./stb_tilemap_editor.wasm', import.meta.url).toString(),
        memoryConfig: {
          initial: 158,   // ~10MB (matches stb_tilemap_editor)
          maximum: 512,   // ~32MB
          shared: true
        }
      })
      this.wasmMemory = initResult.memory

      // 2. Fetch buffer pointers
      const pointerCalls = [
        ['get_cmd_buf_a_ptr'],
        ['get_cmd_buf_b_ptr'],
        ['get_event_buf_ptr'],
        ['get_control_block_ptr'],
        ['get_evt_head_ptr'],
        ['get_cmd_buf_size'],
        ['get_evt_buf_size'],
        ['get_control_block_size']
      ]
      const pointerResults = await this._workerCall('call', { calls: pointerCalls })
      const [
        cmdBufAPtr,
        cmdBufBPtr,
        evtBufPtr,
        controlBlockPtr,
        evtHeadPtr,
        cmdBufSize,
        evtBufSize,
        controlBlockSize
      ] = pointerResults.results

      this.pointers = {
        cmdBufA: cmdBufAPtr,
        cmdBufB: cmdBufBPtr,
        evtBuf: evtBufPtr,
        controlBlock: controlBlockPtr,
        evtHead: evtHeadPtr,
        cmdBufSize,
        evtBufSize,
        controlBlockSize
      }

      // 3. Write map configuration to control block (must be before init)
      const controlView = new Int32Array(this.wasmMemory.buffer, controlBlockPtr, controlBlockSize / 4)
      // Control block layout (uint32 offsets):
      //  9: map_width, 10: map_height, 11: num_layers, 12: spacing_x, 13: spacing_y
      Atomics.store(controlView, 9, 16)   // mapWidth
      Atomics.store(controlView, 10, 16)  // mapHeight
      Atomics.store(controlView, 11, 20)  // numLayers
      Atomics.store(controlView, 12, 16)  // spacing_x
      Atomics.store(controlView, 13, 16)  // spacing_y
      this.tileSpacingX = 16
      this.tileSpacingY = 16

      // 4. Initialize editor
      const initCall = await this._workerCall('call', {
        calls: [['init']]
      })
      if (initCall.results[0] !== 0) {
        throw new Error(`WASM init() failed with code ${initCall.results[0]}`)
      }

      // 5. Load tile images and define tiles
      await this._loadTileSets()

      // 6. Write initial resize event to event buffer
      const evtView = new Int32Array(this.wasmMemory.buffer, evtBufPtr, evtBufSize)

      // Write resize event
      evtView[0] = 5 // EVT_RESIZE
      evtView[1] = 0 // x0
      evtView[2] = 0 // y0
      evtView[3] = w
      evtView[4] = h
      // Update event head
      Atomics.store(controlView, 3, 5) // evt_head = 5 words

      // 5. Run one frame to generate initial draw commands
      await this._workerCall('call', { calls: [['frame']] })

      // 6. Start periodic frame loop (~60fps)
      await this._workerCall('start', { func: 'frame', interval: 16 })

      // 7. Start our render loop
      this.running = true
      this.animFrameId = requestAnimationFrame(this._renderLoop)

      // 8. Send initial resize (in case dimensions changed during load)
      this._handleResize(this.clientWidth, this.clientHeight)

    } catch (err) {
      console.error('Failed to initialize stb-editor:', err)
      this.running = false
      if (this.worker) {
        this.worker.terminate()
        this.worker = null
      }
    }
  }

  _onWorkerMessage(e) {
    const { type, id, ...data } = e.data

    // Handle request/response messages
    if (id !== undefined) {
      const pending = this._pending.get(id)
      if (pending) {
        this._pending.delete(id)
        if (type === 'success') {
          pending.resolve(data)
        } else if (type === 'error') {
          pending.reject(new Error(data.error?.message || String(data.error)))
        } else {
          console.warn('stb-editor: unknown response type:', type, data)
        }
        return
      }
    }

    // Handle unsolicited messages
    switch (type) {
      case 'memory-grown':
        // Memory buffer changed, update our reference
        if (this.wasmMemory && data.buffer) {
          // The buffer property is the same SharedArrayBuffer
          // We need to update ArrayBuffer views in render loop
        }
        break

      default:
        console.warn('stb-editor: unknown worker message:', type, data)
    }
  }

  // ── Render loop ─────────────────────────────────────────────────────

  _renderLoop() {
    if (!this.running) return

    this._renderFrame()
    this.animFrameId = requestAnimationFrame(this._renderLoop)
  }

  _renderFrame() {
    if (!this.wasmMemory || !this.pointers) return

    const { controlBlock, cmdBufA, cmdBufB, cmdBufSize } = this.pointers

    // Read control block via Atomics (shared memory)
    const controlView = new Int32Array(this.wasmMemory.buffer, controlBlock, 16)

    const frameReady = Atomics.load(controlView, CB_FRAME_READY)
    if (!frameReady) return

    // Read which buffer to render from
    const readBuf = Atomics.load(controlView, CB_ACTIVE_READ_BUF)
    const cmdCount = Atomics.load(controlView, readBuf === 0 ? CB_CMD_COUNT_A : CB_CMD_COUNT_B)

    // Get the command buffer view
    const bufPtr = readBuf === 0 ? cmdBufA : cmdBufB
    const cmdView = new Uint32Array(this.wasmMemory.buffer, bufPtr, cmdBufSize)

    // Clear and render
    const ctx = this.ctx
    const dpr = window.devicePixelRatio || 1
    ctx.save()
    ctx.setTransform(1, 0, 0, 1, 0, 0)
    ctx.clearRect(0, 0, this.canvas.width, this.canvas.height)
    // No DPR scaling for stb editor - it works in pixel coordinates
    ctx.restore()

    // Interpret command buffer
    this._executeCommands(ctx, cmdView, cmdCount)

    // Signal we consumed the frame
    Atomics.store(controlView, CB_FRAME_READY, 0)
  }

  /**
   * Interpret the draw command buffer and render to Canvas2D.
   * @param {CanvasRenderingContext2D} ctx
   * @param {Uint32Array} buf - Command buffer
   * @param {number} count - Number of uint32 words used
   */
  _executeCommands(ctx, buf, count) {
    let i = 0
    while (i < count) {
      const type = buf[i]

      switch (type) {
        case CMD_RECT: {
          // Packed format: [CMD_RECT, x0|y0<<16, x1|y1<<16, color]
          const xy0 = buf[i + 1]
          const xy1 = buf[i + 2]
          const color = buf[i + 3]

          const x0 = (xy0 & 0xFFFF) << 16 >> 16 // sign-extend 16-bit
          const y0 = (xy0 >> 16) << 16 >> 16
          const x1 = (xy1 & 0xFFFF) << 16 >> 16
          const y1 = (xy1 >> 16) << 16 >> 16

          const r = (color >> 16) & 0xFF
          const g = (color >> 8) & 0xFF
          const b = color & 0xFF
          ctx.fillStyle = `rgb(${r},${g},${b})`
          ctx.fillRect(x0, y0, x1 - x0, y1 - y0)
          i += 4
          break
        }

        case CMD_TILE: {
          // [CMD_TILE, x0|y0<<16, tile_id, highlight_shifted]
          const xy0 = buf[i + 1]
          const tileId = buf[i + 2]
          const highlight = buf[i + 3] // 0=deemphasize, 1=normal, 2=emphasize

          const x0 = (xy0 & 0xFFFF) << 16 >> 16
          const y0 = (xy0 >> 16) << 16 >> 16

          // Look up tile image
          const tileData = this.tileImages.get(tileId)
          if (tileData) {
            // Apply highlight mode
            if (highlight === 0) {
              ctx.globalAlpha = 0.4 // deemphasize
            } else if (highlight === 2) {
              ctx.globalAlpha = 1.0 // emphasize (brighter)
            } else {
              ctx.globalAlpha = 0.8 // normal
            }
            // Draw image tile
            ctx.drawImage(
              tileData.image,
              tileData.sx, tileData.sy, tileData.sw, tileData.sh,
              x0, y0, this.tileSpacingX, this.tileSpacingY
            )
            // Draw tile ID text (for debugging)
            if (DEBU_TILE_NR) {
              ctx.globalAlpha = 1.0
              ctx.fillStyle = '#ffffff'
              ctx.font = '8px monospace'
              ctx.textAlign = 'center'
              ctx.textBaseline = 'middle'
              ctx.fillText(String(tileId), x0 + this.tileSpacingX / 2, y0 + this.tileSpacingY / 2)
            }
          } else {
            // Fallback: colored rectangle with tile ID
            const colorIdx = tileId % TILE_COLORS.length
            ctx.fillStyle = TILE_COLORS[colorIdx]
            // Apply highlight mode
            if (highlight === 0) {
              ctx.globalAlpha = 0.4
            } else if (highlight === 2) {
              ctx.globalAlpha = 1.0
            } else {
              ctx.globalAlpha = 0.8
            }
            ctx.fillRect(x0, y0, this.tileSpacingX, this.tileSpacingY)
            // Draw tile ID text

            ctx.globalAlpha = 1.0
            ctx.fillStyle = '#ffffff'
            ctx.font = '8px monospace'
            ctx.textAlign = 'center'
            ctx.textBaseline = 'middle'
            ctx.fillText(String(tileId), x0 + this.tileSpacingX / 2, y0 + this.tileSpacingY / 2)
          }

          i += 4
          break
        }

        default:
          // Unknown command or CMD_END - stop processing
          return
      }
    }
  }

  // ── Canvas resize ───────────────────────────────────────────────────

  _handleResize(width, height) {
    if (width === 0 || height === 0) return

    // Set canvas bitmap size (1:1 with CSS pixels for now)
    this.canvas.width = width
    this.canvas.height = height

    // Send resize event to stb editor
    this._pushEvent(EVT_RESIZE, [0, 0, width, height])
  }

  // ── Event forwarding ────────────────────────────────────────────────

  /**
   * Push an event to the shared ring buffer for the WASM editor to consume.
   * @param {number} evtType
   * @param {number[]} fields
   */
  _pushEvent(evtType, fields) {
    if (!this.wasmMemory || !this.pointers) return

    const { evtBuf, controlBlock, evtBufSize } = this.pointers
    const controlView = new Int32Array(this.wasmMemory.buffer, controlBlock, 16)
    const evtView = new Int32Array(this.wasmMemory.buffer, evtBuf, evtBufSize)

    const totalWords = 1 + fields.length // type + fields
    const head = Atomics.load(controlView, CB_EVT_HEAD)
    const tail = Atomics.load(controlView, CB_EVT_TAIL)

    // Check if there's space (leave 1 word gap to distinguish full from empty)
    const used = (head - tail + evtBufSize) % evtBufSize
    if (used + totalWords >= evtBufSize - 1) {
      return // buffer full, drop event
    }

    // Write event
    let pos = head
    evtView[pos % evtBufSize] = evtType
    pos++
    for (const field of fields) {
      evtView[pos % evtBufSize] = field
      pos++
    }

    // Update head atomically
    Atomics.store(controlView, CB_EVT_HEAD, pos)
  }

  /**
   * Convert mouse event to stb editor coordinates.
   * stb_tilemap_editor uses pixel coordinates starting at (0,0) top-left.
   */
  _mouseCoords(e) {
    const rect = this.canvas.getBoundingClientRect()
    return {
      x: Math.round(e.clientX - rect.left),
      y: Math.round(e.clientY - rect.top)
    }
  }

  _onMouseMove(e) {
    const { x, y } = this._mouseCoords(e)
    const shifted = e.shiftKey ? 1 : 0
    const scrollkey = (e.ctrlKey || e.metaKey) ? 1 : 0
    this._pushEvent(EVT_MOUSE_MOVE, [x, y, shifted, scrollkey])
  }

  _onMouseDown(e) {
    this.focus() // grab keyboard focus
    const { x, y } = this._mouseCoords(e)
    const right = e.button === 2 ? 1 : 0
    const shifted = e.shiftKey ? 1 : 0
    const scrollkey = (e.ctrlKey || e.metaKey) ? 1 : 0
    this._pushEvent(EVT_MOUSE_BUTTON, [x, y, right, 1, shifted, scrollkey])
  }

  _onMouseUp(e) {
    const { x, y } = this._mouseCoords(e)
    const right = e.button === 2 ? 1 : 0
    const shifted = e.shiftKey ? 1 : 0
    const scrollkey = (e.ctrlKey || e.metaKey) ? 1 : 0
    this._pushEvent(EVT_MOUSE_BUTTON, [x, y, right, 0, shifted, scrollkey])
  }

  _onWheel(e) {
    e.preventDefault()
    const { x, y } = this._mouseCoords(e)
    // stb expects positive vscroll = scroll up
    const vscroll = -Math.sign(e.deltaY)
    this._pushEvent(EVT_MOUSE_WHEEL, [x, y, vscroll])
  }

  _onContextMenu(e) {
    e.preventDefault() // stb uses right-click for erasing
  }

  _onKeyDown(e) {
    // Map common keyboard shortcuts to stb actions
    // stbte_action enum values from the header:
    //  0=select, 1=brush, 2=erase, 3=rectangle, 4=eyedropper, 5=link
    //  6=toggle_grid, 7=toggle_links, 8=undo, 9=redo
    // 10=cut, 11=copy, 12=paste, 13-16=scroll

    let action = -1
    if (e.ctrlKey || e.metaKey) {
      switch (e.key) {
        case 'z': action = 8; break  // undo
        case 'y': action = 9; break  // redo
        case 'x': action = 10; break // cut
        case 'c': action = 11; break // copy
        case 'v': action = 12; break // paste
      }
    } else {
      switch (e.key) {
        case 's': action = 0; break  // select tool
        case 'b': action = 1; break  // brush tool
        case 'e': action = 2; break  // erase tool
        case 'r': action = 3; break  // rectangle tool
        case 'i': action = 4; break  // eyedropper
        case 'l': action = 5; break  // link tool
        case 'g': action = 6; break  // toggle grid
        case 'ArrowLeft': action = 13; break // scroll left
        case 'ArrowRight': action = 14; break // scroll right
        case 'ArrowUp': action = 15; break // scroll up
        case 'ArrowDown': action = 16; break // scroll down
      }
    }

    if (action >= 0) {
      e.preventDefault()
      this._pushEvent(EVT_ACTION, [action])
    }
  }

  async _loadTileSets() {
    const tileSize = this.tileSpacingX
    const categories = [
      { name: 'floor', file: 'floor-16x16.png', categoryIndex: 3 },
      { name: 'walls_low', file: 'walls_low-16x16.png', categoryIndex: 4 },
      { name: 'walls_high', file: 'walls_high-16x32.png', categoryIndex: 5 }
    ]
    let nextTileId = 1

    for (const cat of categories) {
      const url = new URL(`./example/${cat.file}`, import.meta.url).toString()
      const img = await this._loadImage(url)
      const cols = Math.floor(img.width / tileSize)
      const rows = Math.floor(img.height / tileSize)
      const tileCount = cols * rows
      console.log(`Loading ${cat.name}: ${img.width}x${img.height}, ${cols}x${rows} = ${tileCount} tiles`)
      for (let y = 0; y < rows; y++) {
        for (let x = 0; x < cols; x++) {
          const tileId = nextTileId++
          const result = await this._workerCall('call', {
            calls: [['define_tile', tileId, 0xFF, cat.categoryIndex]]
          })
          if (result.results[0] !== 0) {
            console.error(`Failed to define tile ${tileId} for ${cat.name}: error ${result.results[0]}`)
          }
          this.tileImages.set(tileId, {
            image: img,
            sx: x * tileSize,
            sy: y * tileSize,
            sw: tileSize,
            sh: tileSize
          })
        }
      }
    }
    console.log('Loaded', this.tileImages.size, 'tiles (tile IDs 1..' + (nextTileId-1) + ')')
  }

  _loadImage(url) {
    return new Promise((resolve, reject) => {
      const img = new Image()
      img.onload = () => resolve(img)
      img.onerror = (e) => {
        console.error('Failed to load tile image:', url, e)
        reject(new Error(`Failed to load image: ${url}`))
      }
      img.src = url
    })
  }
}
