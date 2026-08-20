/* Multi-base pointer access to candidate globals.
 * p can point into either a or b, so the access cannot be statically
 * redirected to a single VM shadow. The globals it may alias are excluded
 * from the candidate set and stay in NVM; the compile succeeds. */

int a[4], b[4];
int pick;

int main(void) {
    a[0] = 1;
    b[0] = 2;
    int *p = pick ? a : b;
    return p[0];
}
