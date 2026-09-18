#define CBAPI _cdecl
typedef void (CBAPI *handler)(int code);
extern __declspec(dllimport) int __stdcall win_fn(void);
int (__cdecl fn_ptr_user)(int);
