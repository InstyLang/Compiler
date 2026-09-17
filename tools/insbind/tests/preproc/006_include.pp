#file "widget.h"
3:1 kw_struct struct
3:8 Identifier Widget
3:15 LBrace {
3:17 kw_int int
3:21 Identifier id
3:23 Semicolon ;
3:25 RBrace }
3:26 Semicolon ;
#file "guarded.h"
2:1 kw_int int
2:5 Identifier guarded_value
2:18 Semicolon ;
#file "stddef.h"
8:1 kw_typedef typedef
8:9 kw_unsigned unsigned
8:18 kw_long long
8:23 kw_long long
8:28 Identifier size_t
8:34 Semicolon ;
9:1 kw_typedef typedef
9:9 kw_long long
9:14 kw_long long
9:19 Identifier ptrdiff_t
9:28 Semicolon ;
10:1 kw_typedef typedef
10:9 kw_unsigned unsigned
10:18 kw_short short
10:24 Identifier wchar_t
10:31 Semicolon ;
#file "006_include.c"
6:1 Identifier size_t
6:8 Identifier n
6:10 Assign =
6:12 kw_sizeof sizeof
6:18 LParen (
6:19 kw_int int
6:22 RParen )
6:23 Semicolon ;
