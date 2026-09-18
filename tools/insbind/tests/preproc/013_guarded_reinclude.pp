#file "inner.h"
4:1 kw_int int
4:5 Identifier from_inner
4:16 Assign =
4:18 IntLiteral 1
4:19 Semicolon ;
#file "outer.h"
4:1 kw_int int
4:5 Identifier from_outer
4:16 Assign =
4:18 IntLiteral 1
4:19 Semicolon ;
#file "013_guarded_reinclude.c"
2:1 kw_int int
2:5 Identifier after
2:11 Assign =
2:13 IntLiteral 1
2:14 Semicolon ;
