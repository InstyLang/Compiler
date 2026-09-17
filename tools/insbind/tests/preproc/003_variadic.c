#define COUNT(...) count(__VA_ARGS__)
#define WRAP(name, ...) name(__VA_ARGS__, 0)
int a = COUNT(1, 2, 3);
int b = WRAP(sum, 4, 5);
