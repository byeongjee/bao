/*
 * SCHEMATIC Trace Collection Runtime
 *
 * Linked with instrumented programs to record execution traces at runtime.
 * Writes a JSON trace file (schematic_trace.json) via atexit.
 *
 * The LLVM TraceCollectorPass inserts calls to these functions:
 *   __trace_func_enter(meta)  - on function entry
 *   __trace_bb(bb_idx)        - at each basic block
 *   __trace_loop_enter(id,hdr)- entering a loop (in preheader)
 *   __trace_loop_iter_end(id) - end of loop iteration (in latch)
 *   __trace_loop_exit(id)     - exiting a loop
 *   __trace_func_exit()       - before return
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * FuncTraceMeta struct (must match the LLVM pass layout)
 * ============================================================================ */

typedef struct {
    const char *func_name;
    int bb_count;
    const char **bb_names;
    int loop_count;
    const int *loop_header_bb_idx;
    const char **loop_header_names;
    const char **loop_latch_names;
    const int *loop_depths;
    const int *loop_member_counts;
    const char ***loop_member_names;
    const int *loop_exiting_counts;
    const char ***loop_exiting_names;
} FuncTraceMeta;

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define ITER_BUF_CAP 512      /* max BBs in a single iteration/path buffer */
#define MAX_LOOP_DEPTH 8
#define MAX_FUNCTIONS 32
#define MAX_LOOPS_PER_FUNC 256
#define INIT_TRACE_CAP 64     /* initial capacity for trace arrays (grows) */

/* ============================================================================
 * Data structures (heap-allocated trace storage)
 * ============================================================================ */

typedef struct {
    int *bbs;       /* heap-allocated copy of the path */
    int len;
    int count;
} UniqueTrace;

typedef struct {
    UniqueTrace *traces;
    int count;
    int cap;
} TraceArray;

typedef struct {
    int loop_id;
    int bbs[ITER_BUF_CAP];   /* current iteration buffer (stack-local) */
    int len;
} LoopCtx;

typedef struct {
    int loop_id;
    TraceArray traces;        /* accumulated unique iteration traces */
} LoopResult;

typedef struct {
    const FuncTraceMeta *meta;
    int func_trace_bbs[ITER_BUF_CAP]; /* current function trace buffer */
    int func_trace_len;
    LoopCtx loop_stack[MAX_LOOP_DEPTH];
    int loop_depth;
    TraceArray func_traces;           /* accumulated unique function traces */
    LoopResult loop_results[MAX_LOOPS_PER_FUNC];
    int loop_results_count;
    int loop_results_overflow_reported;
} FuncState;

/* ============================================================================
 * Global state
 * ============================================================================ */

static FuncState g_func_states[MAX_FUNCTIONS];
static int g_func_count = 0;
static FuncState *g_current = NULL;
static int g_atexit_registered = 0;

/* Function call stack: tracks g_current across nested function calls */
#define MAX_CALL_DEPTH 16
static FuncState *g_call_stack[MAX_CALL_DEPTH];
static int g_call_depth = 0;

/* Forward declaration */
static void __trace_write_json(void);

/* ============================================================================
 * TraceArray helpers
 * ============================================================================ */

static void trace_array_init(TraceArray *ta) {
    ta->traces = NULL;
    ta->count = 0;
    ta->cap = 0;
}

static void trace_array_grow(TraceArray *ta) {
    int new_cap = (ta->cap == 0) ? INIT_TRACE_CAP : ta->cap * 2;
    ta->traces = (UniqueTrace *)realloc(ta->traces,
                                         new_cap * sizeof(UniqueTrace));
    ta->cap = new_cap;
}

/* ============================================================================
 * FNV-1a hash for trace deduplication
 * ============================================================================ */

static unsigned fnv1a_hash(const int *data, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned)data[i];
        h *= 16777619u;
    }
    return h;
}

/* ============================================================================
 * Save a trace into a TraceArray (deduplicate by content)
 * ============================================================================ */

static void save_trace(TraceArray *ta, const int *bbs, int len) {
    if (len == 0)
        return;

    /* Check for duplicate */
    unsigned h = fnv1a_hash(bbs, len);
    for (int i = 0; i < ta->count; i++) {
        if (ta->traces[i].len != len)
            continue;
        if (fnv1a_hash(ta->traces[i].bbs, ta->traces[i].len) == h &&
            memcmp(ta->traces[i].bbs, bbs, (size_t)len * sizeof(int)) == 0) {
            ta->traces[i].count++;
            return;
        }
    }

    /* New unique trace — grow if needed */
    if (ta->count >= ta->cap)
        trace_array_grow(ta);

    UniqueTrace *t = &ta->traces[ta->count];
    t->bbs = (int *)malloc((size_t)len * sizeof(int));
    memcpy(t->bbs, bbs, (size_t)len * sizeof(int));
    t->len = len;
    t->count = 1;
    ta->count++;
}

/* ============================================================================
 * Find or create loop result slot by loop_id
 * ============================================================================ */

static LoopResult *get_loop_result(FuncState *fs, int loop_id) {
    for (int i = 0; i < fs->loop_results_count; i++) {
        if (fs->loop_results[i].loop_id == loop_id)
            return &fs->loop_results[i];
    }
    if (fs->loop_results_count >= MAX_LOOPS_PER_FUNC) {
        if (!fs->loop_results_overflow_reported) {
            const char *func_name =
                (fs->meta && fs->meta->func_name) ? fs->meta->func_name : "<unknown>";
            fprintf(stderr,
                    "schematic_trace_runtime: loop result cap (%d) exceeded in %s; "
                    "dropping extra loop traces\n",
                    MAX_LOOPS_PER_FUNC, func_name);
            fs->loop_results_overflow_reported = 1;
        }
        return NULL;
    }
    LoopResult *lr = &fs->loop_results[fs->loop_results_count++];
    lr->loop_id = loop_id;
    trace_array_init(&lr->traces);
    return lr;
}

/* ============================================================================
 * Runtime API
 * ============================================================================ */

void __trace_func_enter(const FuncTraceMeta *meta) {
    if (!g_atexit_registered) {
        atexit(__trace_write_json);
        g_atexit_registered = 1;
    }

    if (g_func_count >= MAX_FUNCTIONS) {
        fprintf(stderr, "schematic_trace_runtime: too many functions\n");
        return;
    }

    /* Push current function state onto the call stack */
    if (g_current && g_call_depth < MAX_CALL_DEPTH)
        g_call_stack[g_call_depth++] = g_current;

    g_current = &g_func_states[g_func_count++];
    g_current->meta = meta;
    g_current->func_trace_len = 0;
    g_current->loop_depth = 0;
    g_current->loop_results_count = 0;
    g_current->loop_results_overflow_reported = 0;
    trace_array_init(&g_current->func_traces);
}

void __trace_bb(int bb_idx) {
    if (!g_current)
        return;

    if (g_current->loop_depth > 0) {
        LoopCtx *lc = &g_current->loop_stack[g_current->loop_depth - 1];
        if (lc->len < ITER_BUF_CAP)
            lc->bbs[lc->len++] = bb_idx;
    } else {
        if (g_current->func_trace_len < ITER_BUF_CAP)
            g_current->func_trace_bbs[g_current->func_trace_len++] = bb_idx;
    }
}

void __trace_loop_enter(int loop_id, int header_bb_idx) {
    if (!g_current)
        return;

    /* Append header to parent trace (function or outer loop) */
    if (g_current->loop_depth > 0) {
        LoopCtx *parent = &g_current->loop_stack[g_current->loop_depth - 1];
        if (parent->len < ITER_BUF_CAP)
            parent->bbs[parent->len++] = header_bb_idx;
    } else {
        if (g_current->func_trace_len < ITER_BUF_CAP)
            g_current->func_trace_bbs[g_current->func_trace_len++] = header_bb_idx;
    }

    /* Push new loop context */
    if (g_current->loop_depth >= MAX_LOOP_DEPTH) {
        fprintf(stderr, "schematic_trace_runtime: loop stack overflow\n");
        return;
    }
    LoopCtx *lc = &g_current->loop_stack[g_current->loop_depth];
    lc->loop_id = loop_id;
    lc->len = 0;
    g_current->loop_depth++;
}

void __trace_loop_iter_end(int loop_id) {
    if (!g_current || g_current->loop_depth == 0)
        return;

    LoopCtx *lc = &g_current->loop_stack[g_current->loop_depth - 1];

    /* Save current iteration to the persistent loop results */
    LoopResult *result = get_loop_result(g_current, loop_id);
    if (result)
        save_trace(&result->traces, lc->bbs, lc->len);

    /* Reset iteration buffer for next iteration */
    lc->len = 0;
}

void __trace_loop_exit(int loop_id) {
    if (!g_current || g_current->loop_depth == 0)
        return;

    /* Discard partial iteration (matches Python: abandon on loop exit) */
    (void)loop_id;
    g_current->loop_depth--;
}

void __trace_func_exit(void) {
    if (!g_current)
        return;

    save_trace(&g_current->func_traces,
               g_current->func_trace_bbs, g_current->func_trace_len);

    /* Reset for potential re-entry */
    g_current->func_trace_len = 0;
    g_current->loop_depth = 0;

    /* Pop caller from the call stack */
    if (g_call_depth > 0)
        g_current = g_call_stack[--g_call_depth];
    else
        g_current = NULL;
}

/* ============================================================================
 * JSON output helpers
 * ============================================================================ */

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

static void write_indent(FILE *fp, int depth) {
    for (int i = 0; i < depth; i++)
        fputs("  ", fp);
}

/* ============================================================================
 * JSON writer (atexit handler)
 * ============================================================================ */

static void __trace_write_json(void) {
    FILE *fp = fopen("schematic_trace.json", "w");
    if (!fp) {
        fprintf(stderr, "schematic_trace_runtime: cannot open schematic_trace.json\n");
        return;
    }

    fputs("{\n", fp);

    for (int fi = 0; fi < g_func_count; fi++) {
        FuncState *fs = &g_func_states[fi];
        const FuncTraceMeta *meta = fs->meta;
        if (!meta)
            continue;

        write_indent(fp, 1);
        write_escaped_string(fp, meta->func_name);
        fputs(": {\n", fp);

        /* Function traces */
        write_indent(fp, 2);
        fputs("\"traces\": [\n", fp);
        for (int ti = 0; ti < fs->func_traces.count; ti++) {
            UniqueTrace *t = &fs->func_traces.traces[ti];
            write_indent(fp, 3);
            fputs("{\"path\": [", fp);
            for (int bi = 0; bi < t->len; bi++) {
                if (bi > 0)
                    fputs(", ", fp);
                int idx = t->bbs[bi];
                if (idx >= 0 && idx < meta->bb_count)
                    write_escaped_string(fp, meta->bb_names[idx]);
                else
                    fprintf(fp, "\"bb_%d\"", idx);
            }
            fprintf(fp, "], \"count\": %d}", t->count);
            if (ti < fs->func_traces.count - 1)
                fputc(',', fp);
            fputc('\n', fp);
        }
        write_indent(fp, 2);
        fputs("],\n", fp);

        /* Loop traces */
        write_indent(fp, 2);
        fputs("\"loop_traces\": {\n", fp);
        for (int li = 0; li < fs->loop_results_count; li++) {
            LoopResult *lr = &fs->loop_results[li];
            int loop_id = lr->loop_id;

            const char *header_name = "unknown";
            if (loop_id < meta->loop_count)
                header_name = meta->loop_header_names[loop_id];

            write_indent(fp, 3);
            write_escaped_string(fp, header_name);
            fputs(": {\n", fp);

            /* Loop metadata */
            write_indent(fp, 4);
            fputs("\"loop\": {\n", fp);

            write_indent(fp, 5);
            fputs("\"header\": ", fp);
            write_escaped_string(fp, header_name);
            fputs(",\n", fp);

            write_indent(fp, 5);
            fputs("\"exiting\": [", fp);
            if (loop_id < meta->loop_count) {
                int ec = meta->loop_exiting_counts[loop_id];
                const char **en = meta->loop_exiting_names[loop_id];
                for (int ei = 0; ei < ec; ei++) {
                    if (ei > 0)
                        fputs(", ", fp);
                    write_escaped_string(fp, en[ei]);
                }
            }
            fputs("],\n", fp);

            write_indent(fp, 5);
            fputs("\"basic_blocks\": [", fp);
            if (loop_id < meta->loop_count) {
                int mc = meta->loop_member_counts[loop_id];
                const char **mn = meta->loop_member_names[loop_id];
                for (int mi = 0; mi < mc; mi++) {
                    if (mi > 0)
                        fputs(", ", fp);
                    write_escaped_string(fp, mn[mi]);
                }
            }
            fputs("],\n", fp);

            write_indent(fp, 5);
            fputs("\"latch\": [", fp);
            if (loop_id < meta->loop_count)
                write_escaped_string(fp, meta->loop_latch_names[loop_id]);
            fputs("],\n", fp);

            write_indent(fp, 5);
            fprintf(fp, "\"depth\": %d\n",
                    (loop_id < meta->loop_count) ? meta->loop_depths[loop_id] : 0);

            write_indent(fp, 4);
            fputs("},\n", fp);

            /* Loop iteration traces */
            write_indent(fp, 4);
            fputs("\"traces\": [\n", fp);
            for (int ti = 0; ti < lr->traces.count; ti++) {
                UniqueTrace *t = &lr->traces.traces[ti];
                write_indent(fp, 5);
                fputs("{\"path\": [", fp);
                for (int bi = 0; bi < t->len; bi++) {
                    if (bi > 0)
                        fputs(", ", fp);
                    int idx = t->bbs[bi];
                    if (idx >= 0 && idx < meta->bb_count)
                        write_escaped_string(fp, meta->bb_names[idx]);
                    else
                        fprintf(fp, "\"bb_%d\"", idx);
                }
                fprintf(fp, "], \"count\": %d}", t->count);
                if (ti < lr->traces.count - 1)
                    fputc(',', fp);
                fputc('\n', fp);
            }
            write_indent(fp, 4);
            fputs("]\n", fp);

            write_indent(fp, 3);
            fputc('}', fp);
            if (li < fs->loop_results_count - 1)
                fputc(',', fp);
            fputc('\n', fp);
        }
        write_indent(fp, 2);
        fputs("}\n", fp);

        write_indent(fp, 1);
        fputc('}', fp);
        if (fi < g_func_count - 1)
            fputc(',', fp);
        fputc('\n', fp);
    }

    fputs("}\n", fp);
    fclose(fp);

    fprintf(stderr, "schematic_trace_runtime: wrote schematic_trace.json "
                    "(%d functions)\n", g_func_count);
}
