#file "009_error_and_warning.c"
6:1 kw_int int
6:5 Identifier after
6:11 Assign =
6:13 IntLiteral 1
6:14 Semicolon ;
! 009_error_and_warning.c:5: #error this is an error
!warn 009_error_and_warning.c:4: #warning this is a warning
