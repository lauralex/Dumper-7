#pragma once

#include <cstdint>
#include <Windows.h>

namespace hv {

inline uint64_t g_cr3 = 0;

inline constexpr uint64_t hypercall_key = 69520;
inline constexpr uint64_t hypervisor_signature = 'fr0a';

struct logger_msg {
  static constexpr uint32_t max_msg_length = 128;
  uint64_t id;
  uint64_t tsc;
  uint32_t aux;
  char data[max_msg_length];
};

enum hypercall_code : uint64_t {
  hypercall_ping = 0,
  hypercall_test,
  hypercall_unload,
  hypercall_read_phys_mem,
  hypercall_write_phys_mem,
  hypercall_read_virt_mem,
  hypercall_write_virt_mem,
  hypercall_query_process_cr3,
  hypercall_install_ept_hook,
  hypercall_remove_ept_hook,
  hypercall_flush_logs,
  hypercall_get_physical_address,
  hypercall_hide_physical_page,
  hypercall_unhide_physical_page,
  hypercall_get_hv_base,
  hypercall_get_hv_size,
  hypercall_get_cpu_base,
  hypercall_install_mmr,
  hypercall_remove_mmr,
  hypercall_remove_all_mmrs,
  hypercall_section_base,
  hypercall_set_section_base,
  hypercall_scan_dtb
};

struct hypercall_input {
  struct {
    hypercall_code code : 8;
    uint64_t       key  : 56;
  };
  uint64_t args[6];
};

uint64_t vmx_vmcall(hypercall_input& input);

inline uint64_t ping() {
  hypercall_input input;
  input.code = hypercall_ping;
  input.key  = hypercall_key;
  return vmx_vmcall(input);
}

inline size_t read_phys_mem(void* const dst, uint64_t const src, size_t const size) {
  hypercall_input input;
  input.code    = hypercall_read_phys_mem;
  input.key     = hypercall_key;
  input.args[0] = reinterpret_cast<uint64_t>(dst);
  input.args[1] = src;
  input.args[2] = size;
  return vmx_vmcall(input);
}

inline size_t write_phys_mem(uint64_t const dst, void const* const src, size_t const size) {
  hypercall_input input;
  input.code    = hypercall_write_phys_mem;
  input.key     = hypercall_key;
  input.args[0] = dst;
  input.args[1] = reinterpret_cast<uint64_t>(src);
  input.args[2] = size;
  return vmx_vmcall(input);
}

inline size_t read_virt_mem(uint64_t const cr3, void* const dst, void const* const src, size_t const size) {
  hypercall_input input;
  input.code    = hypercall_read_virt_mem;
  input.key     = hypercall_key;
  input.args[0] = cr3;
  input.args[1] = reinterpret_cast<uint64_t>(dst);
  input.args[2] = reinterpret_cast<uint64_t>(src);
  input.args[3] = size;
  return vmx_vmcall(input);
}

inline size_t read_virt_mem(void* const dst, void const* const src, size_t const size) {
  return read_virt_mem(g_cr3, dst, src, size);
}

template <typename T>
inline T read_virt_mem(uint64_t const cr3, uintptr_t const src) {
  T buffer = {};
  read_virt_mem(cr3, &buffer, reinterpret_cast<void const*>(src), sizeof(T));
  return buffer;
}

template <typename T>
inline T read_virt_mem(uintptr_t const src) {
  return read_virt_mem<T>(g_cr3, src);
}

inline size_t write_virt_mem(uint64_t const cr3, void* const dst, void const* const src, size_t const size) {
  hypercall_input input;
  input.code    = hypercall_write_virt_mem;
  input.key     = hypercall_key;
  input.args[0] = cr3;
  input.args[1] = reinterpret_cast<uint64_t>(dst);
  input.args[2] = reinterpret_cast<uint64_t>(src);
  input.args[3] = size;
  return vmx_vmcall(input);
}

inline uint64_t query_process_cr3(uint64_t const pid) {
  hypercall_input input;
  input.code    = hypercall_query_process_cr3;
  input.key     = hypercall_key;
  input.args[0] = pid;
  return vmx_vmcall(input);
}

inline uint64_t get_physical_address(uint64_t const cr3, void const* const address) {
  hypercall_input input;
  input.code    = hypercall_get_physical_address;
  input.key     = hypercall_key;
  input.args[0] = cr3;
  input.args[1] = reinterpret_cast<uint64_t>(address);
  return vmx_vmcall(input);
}

inline uint64_t get_section_base_process(DWORD const pid) {
  hypercall_input input;
  input.code    = hypercall_section_base;
  input.key     = hypercall_key;
  input.args[0] = pid;
  return vmx_vmcall(input);
}

inline uint64_t scan_process_dtb(DWORD const pid) {
  hypercall_input input;
  input.code    = hypercall_scan_dtb;
  input.key     = hypercall_key;
  input.args[0] = pid;
  return vmx_vmcall(input);
}

} // namespace hv
