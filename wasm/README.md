# WebAssembly build

The FastBook engine compiled to WASM with Emscripten/embind — the same C++
that produces the native benchmark numbers, running in the browser. A live
playground (interactive book + JS-vs-WASM benchmark) is at
**[dmitridefreitas.com/lab/wasm-engine](https://dmitridefreitas.com/lab/wasm-engine)**.

- `bindings.cpp` — embind wrapper: interactive `WasmBook` (auto-assigned
  order ids, fill reporting, level snapshots) and `runBenchmark()`, which
  replays the exact `generate_flow()` stream used by the native benches so
  browser and native numbers are directly comparable.
- `lob_engine.js` — committed build artifact (`SINGLE_FILE`, ES6 module,
  works in web and Node). Rebuild with the command at the top of
  `bindings.cpp` (emsdk required).

Reference point: the canonical 200k-op flow runs at ~10M ops/s under WASM in
Node/V8 on the same machine that measures 14.1M ops/s native — WebAssembly
keeps roughly 70% of native throughput on this workload.
