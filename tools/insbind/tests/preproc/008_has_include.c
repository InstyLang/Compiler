#if __has_include("definitely_absent_local.h")
# error absent header reported present
#endif
#if __has_include(<stddef.h>)
int have_stddef = 1;
#else
# error stddef.h missing
#endif
#if __has_include(<nonexistent_header_xyz.h>)
int bogus = 1;
#endif
