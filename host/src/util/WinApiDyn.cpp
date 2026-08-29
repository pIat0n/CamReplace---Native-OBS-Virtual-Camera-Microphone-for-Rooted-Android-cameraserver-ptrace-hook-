#include "util/WinApiDyn.h"

namespace cr::winapi
{
namespace
{

constexpr NTSTATUS kStatusEntryPointNotFound = static_cast<NTSTATUS>(0xC0000139L);

FARPROC resolve_raw(const char* dll, const char* name) noexcept
{
  HMODULE module = ::LoadLibraryA(dll);
  return module ? ::GetProcAddress(module, name) : nullptr;
}

template <typename Fn> Fn resolve(const char* dll, const char* name) noexcept
{
  return reinterpret_cast<Fn>(resolve_raw(dll, name));
}

#define CR_DYN_PROC(dll_name, proc_name) \
  using Fn = decltype(&proc_name); \
  static Fn fn = resolve<Fn>(dll_name, #proc_name)

} // namespace

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG bytes, ULONG flags) noexcept
{
  CR_DYN_PROC("bcrypt.dll", BCryptGenRandom);
  return fn ? fn(algorithm, buffer, bytes, flags) : kStatusEntryPointNotFound;
}

HINSTANCE WINAPI ShellExecuteW(HWND window, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT show_command) noexcept
{
  CR_DYN_PROC("shell32.dll", ShellExecuteW);
  return fn ? fn(window, operation, file, parameters, directory, show_command) : reinterpret_cast<HINSTANCE>(SE_ERR_DLLNOTFOUND);
}

HRESULT WINAPI SHGetKnownFolderPath(REFKNOWNFOLDERID folder_id, DWORD flags, HANDLE token, PWSTR* path) noexcept
{
  CR_DYN_PROC("shell32.dll", SHGetKnownFolderPath);
  return fn ? fn(folder_id, flags, token, path) : E_FAIL;
}

VOID WINAPI OutputDebugStringA(LPCSTR text) noexcept
{
  CR_DYN_PROC("kernel32.dll", OutputDebugStringA);
  if (fn)
    fn(text);
}

LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR subkey, DWORD options, REGSAM access, PHKEY result) noexcept
{
  CR_DYN_PROC("advapi32.dll", RegOpenKeyExW);
  return fn ? fn(key, subkey, options, access, result) : ERROR_PROC_NOT_FOUND;
}

LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD bytes) noexcept
{
  CR_DYN_PROC("advapi32.dll", RegQueryValueExW);
  return fn ? fn(key, value, reserved, type, data, bytes) : ERROR_PROC_NOT_FOUND;
}

LSTATUS WINAPI RegCloseKey(HKEY key) noexcept
{
  CR_DYN_PROC("advapi32.dll", RegCloseKey);
  return fn ? fn(key) : ERROR_PROC_NOT_FOUND;
}

BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, PHANDLE token) noexcept
{
  CR_DYN_PROC("advapi32.dll", OpenProcessToken);
  return fn ? fn(process, access, token) : FALSE;
}

BOOL WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS info_class, LPVOID info, DWORD info_bytes, PDWORD returned_bytes) noexcept
{
  CR_DYN_PROC("advapi32.dll", GetTokenInformation);
  return fn ? fn(token, info_class, info, info_bytes, returned_bytes) : FALSE;
}

#undef CR_DYN_PROC

} // namespace cr::winapi
