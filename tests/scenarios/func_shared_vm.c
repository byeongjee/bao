// Inter-procedural fixture: the SAME global is hot (VM-placed) in two functions.
//
// `a` is read/written several times in both f and main, so SCHEMATIC places it
// in VM (SRAM) in each. Under the module pass these two per-function placements
// must share ONE module-scoped shadow global (__vm_shadow_a). If each function
// created its own shadow, f and main would cache `a` in different SRAM locations
// and a transparent (DISABLED) call would read a stale value.

int a = 0;

__attribute__((noinline)) int f(void) {
    a = a + 1;
    a = a * 2;
    a = a - 3;
    a = a + 4;
    return a;
}

int main(void) {
    a = a + 5;
    a = a * 6;
    a = a - 7;
    a = a + 8;
    a = f();
    a = a + 9;
    a = a * 10;
    a = a - 11;
    return a;
}
