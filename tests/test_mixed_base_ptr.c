/* Multi-base pointer access to candidate globals.
 * p can point into either a or b, so the access cannot be statically
 * redirected to a single VM shadow. Strict mode must fail the compile
 * with an explicit error instead of silently skipping the function. */

int a[4], b[4];
int pick;

int main(void) {
    a[0] = 1;
    b[0] = 2;
    int *p = pick ? a : b;
    return p[0];
}
