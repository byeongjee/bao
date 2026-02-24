/*
 * BB Frequency Collection Runtime
 *
 * Linked with instrumented programs to record per-BB visit counts at runtime.
 * Writes a JSON file (bb_freq.json) when __bb_freq_dump() is called.
 *
 * The LLVM BBFreqCollectorPass inserts:
 *   __bb_freq_register(func_name, bb_names, bb_count, counters) - at function entry
 *   counter[bb_idx] += 1                                        - at each BB (inline)
 *   __bb_freq_dump()                                            - before return in main()
 */

#include <stdio.h>
#include <string.h>

#define MAX_FUNCTIONS 64

typedef struct {
    const char *func_name;
    const char **bb_names;
    int bb_count;
    long long *counters;
} BBFreqRegistration;

static BBFreqRegistration g_registrations[MAX_FUNCTIONS];
static int g_reg_count = 0;

void __bb_freq_register(const char *func_name, const char **bb_names,
                        int bb_count, long long *counters) {
    if (g_reg_count >= MAX_FUNCTIONS) {
        fprintf(stderr, "bb_freq_runtime: too many functions (max %d)\n",
                MAX_FUNCTIONS);
        return;
    }

    /* Avoid duplicate registration (function may be called multiple times) */
    for (int i = 0; i < g_reg_count; i++) {
        if (strcmp(g_registrations[i].func_name, func_name) == 0)
            return;
    }

    BBFreqRegistration *reg = &g_registrations[g_reg_count++];
    reg->func_name = func_name;
    reg->bb_names = bb_names;
    reg->bb_count = bb_count;
    reg->counters = counters;
}

static void write_escaped_string(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:   fputc(*s, fp); break;
        }
    }
    fputc('"', fp);
}

void __bb_freq_dump(void) {
    FILE *fp = fopen("bb_freq.json", "w");
    if (!fp) {
        fprintf(stderr, "bb_freq_runtime: cannot open bb_freq.json for writing\n");
        return;
    }

    fputs("{\n", fp);

    for (int fi = 0; fi < g_reg_count; fi++) {
        BBFreqRegistration *reg = &g_registrations[fi];

        fputs("  ", fp);
        write_escaped_string(fp, reg->func_name);
        fputs(": {\n", fp);

        for (int bi = 0; bi < reg->bb_count; bi++) {
            fputs("    ", fp);
            write_escaped_string(fp, reg->bb_names[bi]);
            fprintf(fp, ": %lld", reg->counters[bi]);
            if (bi < reg->bb_count - 1)
                fputc(',', fp);
            fputc('\n', fp);
        }

        fputs("  }", fp);
        if (fi < g_reg_count - 1)
            fputc(',', fp);
        fputc('\n', fp);
    }

    fputs("}\n", fp);
    fclose(fp);

    fprintf(stderr, "bb_freq_runtime: wrote bb_freq.json (%d functions)\n",
            g_reg_count);
}
