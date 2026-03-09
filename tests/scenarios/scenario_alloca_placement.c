/* SCHEMATIC O0 alloca placement test.
 * At O0, local variables are alloca+load/store pairs.
 * With SchematicStateAnalysis, these allocas become placement candidates.
 * The optimizer can place frequently-accessed allocas in VM (SRAM).
 * Expected: __vm_shadow_ created for hot allocas placed in VM. */

int g_result;

int main(void) {
    int x = 10;
    int y = 20;
    int z = x + y;
    g_result = z;
    int w = z * 2;
    g_result = w;
    return 0;
}
