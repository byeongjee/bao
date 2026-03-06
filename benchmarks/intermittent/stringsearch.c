/*
** Pratt-Boyer-Moore string search benchmark.
** Preprocessed for intermittent computing checkpoint insertion.
** Original by Jerry Coffin (1991), Public Domain.
*/

#include <stdint.h>
#include "loop_tripcount.h"

#define FORCE_INLINE static inline __attribute__((always_inline))

// --- Configuration ---
#define NUM_PATTERNS 26
#define TEXT_LEN 512

// --- Mutable globals (MILP candidates) ---

static uint32_t table[256] __attribute__((used, annotate("milp_candidate")));
static uint32_t len __attribute__((used, annotate("milp_candidate")));
static char *findme __attribute__((used, annotate("milp_candidate")));

// --- Const data ---

static const char *find_strings[NUM_PATTERNS] = {
    "Kur",
    "gent",
    "lass",
    "suns",
    "for",
    "xxx",
    "long",
    "have",
    "where",
    "xxxxxx",
    "xxxxxx",
    "pense",
    "pow",
    "xxxxx",
    "Yo",
    "and",
    "faded",
    "20",
    "you",
    "bac",
    "an",
    "way",
    "possibili",
    "fat",
    "imag",
    "th"
};

static const char search_text[] =
    "KurtVonnegutsCommencementAddressatMITLadiesandgentlemen"
    "oftheclassof97WearsunscreenIfIcouldofferyouonlyonetip"
    "forthefuturesunscreenwouldbeitThelongtermbenefitsof"
    "sunscreenhavebeenprovedbyscientistswhereastherestof"
    "myadvicehasnobasismorereliablethanmyownmeandering"
    "experienceIwilldispensethisadvicenowEnjoythepowerand"
    "beautyofyouryouthOhnevermindYouwillnotunderstandthe"
    "powerandbeautyofyouryouthuntiltheyvefadedButtrustme"
    "in20yearsyoulllookbackatphotosofyourselfandrecallin"
    "awayyoucantgraspnowhowmuchpossibilitylaybefore";

// --- Helper functions ---

FORCE_INLINE void init_search(const char *pattern) {
    uint32_t i;

    // Compute pattern length
    len = 0;
    const char *p = pattern;
    while (*p) {
        __loop_tripcount(16);
        len++;
        p++;
    }

    // Fill table with default skip distance
    for (i = 0; i < 256; i++) {
        __loop_tripcount(256);
        table[i] = len;
    }

    // Set skip distances for pattern characters
    for (i = 0; i < len; i++) {
        __loop_tripcount(16);
        table[(unsigned char)pattern[i]] = len - i - 1;
    }

    findme = (char *)pattern;
}

FORCE_INLINE const char *strsearch(const char *pattern, const char *text) {
    uint32_t shift = 0;
    uint32_t pos = len - 1;
    uint32_t limit = 0;

    // Compute text length
    const char *tp = text;
    while (*tp) {
        __loop_tripcount(TEXT_LEN);
        limit++;
        tp++;
    }

    while (pos < limit) {
        __loop_tripcount(TEXT_LEN);

        // Skip loop: advance by table lookup
        while (pos < limit &&
               (shift = table[(unsigned char)text[pos]]) > 0) {
            __loop_tripcount(TEXT_LEN);
            pos += shift;
        }

        if (shift == 0) {
            // Check for match
            const char *here = &text[pos - len + 1];
            const char *a = findme;
            const char *b = here;
            uint32_t matched = 1;
            uint32_t j;
            for (j = 0; j < len; j++) {
                __loop_tripcount(16);
                if (a[j] != b[j]) {
                    matched = 0;
                    break;
                }
            }
            if (matched) {
                return here;
            } else {
                pos++;
            }
        }
    }
    return (const char *)0;
}

// --- Main ---

__attribute__((noinline)) int main(void) {
    volatile int total_found = 0;
    int i;

    for (i = 0; i < NUM_PATTERNS; i++) {
        __loop_tripcount(NUM_PATTERNS);
        findme = (char *)find_strings[i];

        // Compute pattern length manually (no strlen)
        len = 0;
        const char *p = findme;
        while (*p) {
            __loop_tripcount(16);
            len++;
            p++;
        }

        init_search(findme);
        const char *result = strsearch(findme, search_text);
        if (result) {
            total_found++;
        }
    }

    return total_found;
}
