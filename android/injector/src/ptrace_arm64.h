#pragma once

// Small ARM64-only ptrace wrapper used by cr_injector.
// Scope is intentionally narrow:
//   * attach / detach
//   * read + write registers (PTRACE_GETREGSET / SETREGSET with NT_PRSTATUS)
//   * read + write remote memory (PEEKDATA / POKEDATA — 8-byte chunks)
//   * call a remote function with up to 4 register-passed args
// Everything returns 0 on success, -errno on failure, and logs via android_log.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/types.h>
namespace cr::inj
{

struct RemoteRegs
{
  uint64_t x[31]; // x0..x30
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
};
static_assert(sizeof(RemoteRegs) == 34 * 8, "fixed layout" /*OBF_SKIP*/);

// Attach and wait for the target to enter stopped state.
int attach(pid_t pid);
// Detach (cannot fail fatally — we always try to unblock the target).
void detach(pid_t pid);

int get_regs(pid_t pid, RemoteRegs& out);
int set_regs(pid_t pid, const RemoteRegs& r);

// Remote memory I/O. `addr` must be aligned to 8 bytes; len is rounded up
// internally. Returns 0 on success.
int peek_mem(pid_t pid, uintptr_t addr, void* out, size_t len);
int poke_mem(pid_t pid, uintptr_t addr, const void* in, size_t len);

// Run `fn(arg0..arg3)` inside the target and block until it reaches the
// controlled return trap. Every call starts from `base_regs` so SP/PC/LR from
// a previous trap never leak into the next call. With return_trap=0 success is
// only a null-return SIGSEGV/SIGBUS, not any crash-like signal.
int remote_call(pid_t pid, const RemoteRegs& base_regs, const char* stage, uintptr_t fn, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t return_trap, uintptr_t call_sp, uint64_t* ret_x0);

// Find the writable mapping containing `addr`, usually the target stack.
bool find_writable_mapping(pid_t pid, uintptr_t addr, uintptr_t& start, uintptr_t& end);

// Look up a .dynsym/.symtab symbol in the target. Parses /proc/pid/maps to
// locate the SO, reads its ELF header via peek_mem, walks symbol tables.
// Returns the symbol's absolute address in target memory, or 0.
uintptr_t lookup_symbol(pid_t pid, const char* so_basename, const char* symbol);

// Handy: find a PID by short process name via /proc/*/comm.
pid_t find_pid(const char* comm_name);

} // namespace cr::inj
