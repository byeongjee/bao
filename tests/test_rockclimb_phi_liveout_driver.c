/*
 * Runtime driver for PHI-defined live-out checkpoint test.
 *
 * Provides runtime stubs (__rockclimb_check, __rockclimb_save_reg, __nvm_regs)
 * and a main() that:
 *   1. Runs the instrumented function (golden result)
 *   2. Checks whether the PHI-defined value was saved to NVM
 *   3. Simulates a power-failure restart using only NVM-saved values
 *   4. Compares reconstructed result against golden
 *
 * If the PHI value was NOT checkpointed, the reconstructed result is WRONG
 * -- demonstrating data loss on power failure.
 */

#include <stdio.h>
#include <string.h>

/* NVM storage -- the only state that survives power failure */
#define NVM_SIZE 32
int __nvm_regs[NVM_SIZE];

/* Track every __rockclimb_save_reg call */
#define MAX_SAVES 64
static int save_ids[MAX_SAVES];
static int save_vals[MAX_SAVES];
static int save_count = 0;

/* Runtime stubs */
void __rockclimb_check(void) {
    /* no-op: boundary marker only */
}

void __rockclimb_save_reg(int id, int val) {
    if (id < NVM_SIZE)
        __nvm_regs[id] = val;
    if (save_count < MAX_SAVES) {
        save_ids[save_count] = id;
        save_vals[save_count] = val;
        save_count++;
    }
}

/* Functions from the instrumented test module */
extern int test_rockclimb_phi_liveout(int x, int cond);
extern int expensive_a(int x);

int main(void) {
    int x = 42;
    int cond = 1;  /* then path: PHI resolves to expensive_a(x) */

    /*
     * Why cond=1?  LLVM's RPO puts if.else in its own region and if.then
     * in the same region as if.end (the merge block).  With cond=0 the
     * PHI input (%call1 from if.else) is independently live-out and saved,
     * masking the bug.  With cond=1 the PHI input (%call from if.then) is
     * in the same region as the merge block — NOT independently live-out.
     * Only if the PHI node itself is treated as a definition will it be
     * checkpointed.
     */

    memset(__nvm_regs, 0, sizeof(__nvm_regs));
    save_count = 0;

    /* ---- Golden run ---- */
    int golden = test_rockclimb_phi_liveout(x, cond);

    /* Independently compute the expected PHI value.
     * expensive_a has 0 checkpoints so calling it won't affect NVM state. */
    int expected_phi = expensive_a(x);

    /* ---- Check NVM for the PHI value ---- */
    int phi_in_nvm = 0;
    for (int i = 0; i < save_count; i++) {
        if (save_vals[i] == expected_phi) {
            phi_in_nvm = 1;
            break;
        }
    }

    /* Find w = (result + 1) * 2 in NVM (non-PHI value, always checkpointed) */
    int expected_w = (expected_phi + 1) * 2;
    int w_in_nvm = 0;
    for (int i = 0; i < save_count; i++) {
        if (save_vals[i] == expected_w) {
            w_in_nvm = 1;
            break;
        }
    }

    /* ---- Simulate power-failure restart ----
     * Only NVM-saved values survive. Post-boundary code does:
     *   z = checkpoint_barrier(w);  // z = w
     *   return result + z;
     */
    int restored_phi = phi_in_nvm ? expected_phi : 0;
    int restored_w   = w_in_nvm   ? expected_w   : 0;
    int restored_result = restored_phi + restored_w;

    /* ---- Report ---- */
    printf("=== PHI Live-Out Runtime Test ===\n");
    printf("Input: x=%d, cond=%d\n", x, cond);
    printf("Golden result:              %d\n", golden);
    printf("Expected PHI value:         %d\n", expected_phi);
    printf("NVM saves (%d total):\n", save_count);
    for (int i = 0; i < save_count; i++) {
        const char *tag = "";
        if (save_vals[i] == expected_phi) tag = " <-- PHI";
        if (save_vals[i] == expected_w)   tag = " <-- w";
        printf("  nvm_reg[%d] = %d%s\n", save_ids[i], save_vals[i], tag);
    }
    printf("PHI in NVM:                 %s\n", phi_in_nvm ? "yes" : "NO");
    printf("Result after power failure: %d\n", restored_result);

    if (restored_result == golden) {
        printf("\nPASS: power failure preserves correct result\n");
        return 0;
    } else {
        printf("\nFAIL: power failure gives %d instead of %d\n",
               restored_result, golden);
        printf("  PHI-defined value was not saved to NVM -- data loss on reboot.\n");
        return 1;
    }
}
