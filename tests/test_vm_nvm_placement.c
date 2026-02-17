/*
 * Test: VM/NVM Memory Placement
 * Tests that the MILP assigns globals to VM (SRAM) or NVM (FRAM)
 * based on access frequency and capacity constraints.
 *
 * Two globals: one frequently accessed, one rarely accessed.
 * The optimizer should prefer placing frequently-accessed globals in VM.
 */

int frequently_accessed __attribute__((annotate("milp_candidate")));
int rarely_accessed __attribute__((annotate("milp_candidate")));

int test_vm_nvm_placement(int x) {
    // Frequent accesses to first global
    frequently_accessed = x;
    int a = frequently_accessed + 1;
    frequently_accessed = a;
    int b = frequently_accessed + 2;
    frequently_accessed = b;
    int c = frequently_accessed + 3;

    // Single access to second global
    rarely_accessed = c;

    return c + rarely_accessed;
}
