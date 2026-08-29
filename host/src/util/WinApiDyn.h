#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

namespace cr::winapi
{

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG bytes, ULONG flags) noexcept;
HINSTANCE WINAPI ShellExecuteW(HWND window, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT show_command) noexcept;
HRESULT WINAPI SHGetKnownFolderPath(REFKNOWNFOLDERID folder_id, DWORD flags, HANDLE token, PWSTR* path) noexcept;
VOID WINAPI OutputDebugStringA(LPCSTR text) noexcept;
LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR subkey, DWORD options, REGSAM access, PHKEY result) noexcept;
LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD bytes) noexcept;
LSTATUS WINAPI RegCloseKey(HKEY key) noexcept;
BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, PHANDLE token) noexcept;
BOOL WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS info_class, LPVOID info, DWORD info_bytes, PDWORD returned_bytes) noexcept;

} // namespace cr::winapi
