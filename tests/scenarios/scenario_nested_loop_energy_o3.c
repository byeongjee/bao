/* Regression test for optimized nested-loop energy propagation.
 * Volatile loop bounds keep the loops alive under O3 while tripcount
 * annotations still provide the worst-case bounds to SCHEMATIC. */

void __loop_tripcount(int);

volatile int g_outer_bound = 1;
volatile int g_inner_bound = 100;
int g_result;

int main(void) {
    int sum = 0;

    for (int i = 0; i < g_outer_bound; i++) {
        __loop_tripcount(1);
        for (int j = 0; j < g_inner_bound; j++) {
            __loop_tripcount(100);
            sum += j * 3 + i;
        }
    }

    g_result = sum;
    return 0;
}
