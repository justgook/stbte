/**
 * Generic WASM Worker
 *
 * Provides a reusable Web Worker for WebAssembly modules with shared memory.
 * Supports init, batch function calls, and periodic execution.
 *
 * Message protocol:
 *   init  -> Load WASM module, return memory and export names
 *   call  -> Execute one or more WASM exports, return results
 *   start -> Begin periodic execution of a WASM export
 *   stop  -> Halt periodic execution
 *
 * All messages include an `id` field for request/response correlation.
 * Caller-provided IDs can be any serializable value (number, string).
 *
 * Shared memory is enabled by default for high-performance applications.
 */

/** @type {WebAssembly.Instance|null} */
let wasmInstance = null

/** @type {WebAssembly.Memory|null} */
let wasmMemory = null

/** @type {number|null} */
let timerId = null

/** @type {string|null} */
let timerFunc = null

/** @type {number[]|null} */
let timerArgs = null

/** @type {number} */
let timerInterval = 0

/**
 * Load and instantiate a WASM module.
 * @param {string} wasmUrl - URL to fetch the WASM binary
 * @param {Object} [memoryConfig] - Memory configuration
 * @param {number} [memoryConfig.initial=158] - Initial pages (64KB each)
 * @param {number} [memoryConfig.maximum=512] - Maximum pages
 * @param {boolean} [memoryConfig.shared=true] - Use SharedArrayBuffer
 * @param {Object} [importObject] - Additional WASM imports
 * @returns {Promise<{memory: WebAssembly.Memory, exports: string[]}>}
 */
async function initWasm(wasmUrl, memoryConfig = {}, importObject = {}) {
  // Default memory configuration (matches stb_tilemap_editor)
  const config = {
    initial: 158,   // ~10MB
    maximum: 512,   // ~32MB
    shared: true,
    ...memoryConfig
  }

  // Fetch WASM binary
  const response = await fetch(wasmUrl)
  if (!response.ok) {
    throw new Error(`Failed to fetch WASM: ${response.status} ${response.statusText}`)
  }
  const wasmBytes = await response.arrayBuffer()

  // Create memory
  wasmMemory = new WebAssembly.Memory(config)

  // Prepare imports with memory
  const imports = {
    env: {
      memory: wasmMemory,
      ...(importObject.env || {})
    },
    ...importObject
  }
  // Ensure memory is in env
  if (!imports.env.memory) {
    imports.env.memory = wasmMemory
  }

  // Instantiate
  const { instance } = await WebAssembly.instantiate(wasmBytes, imports)
  wasmInstance = instance

  // Collect export names (functions only)
  const exports = []
  for (const [name, desc] of Object.entries(instance.exports)) {
    if (typeof desc === 'function') {
      exports.push(name)
    }
  }

  return { memory: wasmMemory, exports }
}

/**
 * Execute a batch of WASM function calls.
 * @param {Array<Array>} calls - Array of [name, ...args]
 * @returns {Array<number|null>} Results (null for void returns)
 */
function executeCalls(calls) {
  if (!wasmInstance) {
    throw new Error('WASM not initialized')
  }

  const results = []
  for (const call of calls) {
    if (!Array.isArray(call) || call.length === 0) {
      throw new Error('Invalid call format: expected [name, ...args]')
    }

    const [name, ...args] = call
    const func = wasmInstance.exports[name]
    if (typeof func !== 'function') {
      throw new Error(`Export '${name}' is not a function`)
    }

    // Call WASM function with numeric arguments
    const result = func(...args)

    // Convert undefined/void to null
    results.push(result === undefined ? null : result)
  }

  return results
}

/**
 * Start periodic execution of a WASM function.
 * @param {string} funcName - Export name to call
 * @param {number} interval - Milliseconds (0 = immediate loop)
 * @param {number[]} [args] - Arguments to pass each call
 */
function startTimer(funcName, interval, args = []) {
  if (!wasmInstance) {
    throw new Error('WASM not initialized')
  }

  const func = wasmInstance.exports[funcName]
  if (typeof func !== 'function') {
    throw new Error(`Export '${funcName}' is not a function`)
  }

  timerFunc = funcName
  timerArgs = args
  timerInterval = interval

  if (interval > 0) {
    timerId = setInterval(() => {
      try {
        func(...args)
      } catch (err) {
        console.error('WASM timer error:', err)
        stopTimer()
      }
    }, interval)
  } else {
    // Immediate loop (as fast as possible)
    timerId = 0 // special marker
    const loop = () => {
      if (timerId === 0) { // still running
        try {
          func(...args)
          setTimeout(loop, 0)
        } catch (err) {
          console.error('WASM loop error:', err)
          stopTimer()
        }
      }
    }
    loop()
  }
}

/**
 * Stop periodic execution.
 */
function stopTimer() {
  if (timerId !== null) {
    if (timerId > 0) {
      clearInterval(timerId)
    }
    timerId = null
    timerFunc = null
    timerArgs = null
    timerInterval = 0
  }
}

/**
 * Send success response.
 * @param {any} id - Request correlation ID
 * @param {Object} data - Response payload
 */
function postSuccess(id, data) {
  self.postMessage({ type: 'success', id, ...data })
}

/**
 * Send error response.
 * @param {any} id - Request correlation ID
 * @param {Error|string} error - Error object or message
 */
function postError(id, error) {
  const err = error instanceof Error ? error : new Error(String(error))
  self.postMessage({
    type: 'error',
    id,
    error: {
      message: err.message,
      stack: err.stack,
      name: err.name
    }
  })
}

// Message handler
self.onmessage = async (e) => {
  const { type, id, ...payload } = e.data

  try {
    switch (type) {
      case 'init': {
        const { wasmUrl, memoryConfig, importObject } = payload
        const { memory, exports } = await initWasm(wasmUrl, memoryConfig, importObject)
        postSuccess(id, { memory, exports })
        break
      }

      case 'call': {
        const { calls } = payload
        if (!Array.isArray(calls)) {
          throw new Error('Missing or invalid calls array')
        }
        const results = executeCalls(calls)
        postSuccess(id, { results })
        break
      }

      case 'start': {
        const { func, interval = 16, args = [] } = payload
        if (typeof func !== 'string') {
          throw new Error('Missing or invalid func name')
        }
        startTimer(func, interval, args)
        postSuccess(id, {})
        break
      }

      case 'stop': {
        stopTimer()
        postSuccess(id, {})
        break
      }

      default:
        throw new Error(`Unknown message type: ${type}`)
    }
  } catch (err) {
    postError(id, err)
  }
}

// Notify main thread when memory grows (SharedArrayBuffer)
if (typeof SharedArrayBuffer !== 'undefined') {
  const originalGrow = WebAssembly.Memory.prototype.grow
  WebAssembly.Memory.prototype.grow = function(pages) {
    const result = originalGrow.call(this, pages)
    if (this === wasmMemory) {
      self.postMessage({
        type: 'memory-grown',
        pages: result,
        buffer: this.buffer
      })
    }
    return result
  }
}