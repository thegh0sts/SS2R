#include "runtime.h"
#include <stdint.h>
#include <stddef.h>

// --- bbgpu Stubs with strict ABI Alignment ---
ABI void bbgpu_register_kernel(void* a) {}
ABI int bbgpu_init(void* a) { return 0; }
ABI size_t bbgpu_symbol_count(void) { return 0; }
ABI void bbgpu_patch_image(void* a, void* b) {}
ABI int bbgpu_handle_fault(void* a) { return 0; }
ABI void bbgpu_dump_guest_writes(void) {}
ABI uintptr_t bbgpu_resolve(const char* name) { return 0; }
ABI int bbgpu_overlay_captures_input(void) { return 0; }
ABI void* bbgpu_pad_rescan_take(void) { return NULL; }
ABI void bbgpu_text_input_begin(void* a) {}
ABI int bbgpu_text_input_poll(void* a) { return 0; }

// --- Atrac9 Audio Stubs with strict ABI Alignment ---
ABI int Atrac9ReleaseHandle(int handle) { return 0; }
ABI int Atrac9Decode(int handle, void* in, void* out, void* opts) { return 0; }
ABI int Atrac9GetHandle(void* init_params) { return 1; }
ABI int Atrac9InitDecoder(int handle, void* config) { return 0; }
ABI int Atrac9GetCodecInfo(int handle, void* info) { return 0; }