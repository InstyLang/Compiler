__declspec(dllimport) int imported_fn(int x);
extern __declspec(dllimport) int imported_var;
int __attribute__((visibility("default"))) visible(void);
void __stdcall win_style(int a, int b);
int __cdecl c_style(void);
static inline int max2(int a, int b) { return a > b ? a : b; }
static inline void with_body(void) {
    int braces = 0;
    if (braces) { braces++; } else { braces--; }
}
int after(void);
