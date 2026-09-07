/* Exercise the actual pinned selector with synthetic CPUID sets. No selected
 * vector routine executes, so partial-AVX512 cases work on any test host. */
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <immintrin.h>
#include <string.h>
#include <stddef.h>
static unsigned test_features;
static int supports(const char *feature) {
    const char *names[] = {"ssse3", "avx2", "avx512f", "avx512bw", "avx512dq", "avx512vl", "gfni"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(feature, names[i])) return (test_features & (1u << i)) != 0;
    return 0;
}
#define __builtin_cpu_supports(feature) supports(feature)
#define __builtin_cpu_init() ((void)0)
#define oblas_get_impl nanors_test_get_impl
#include "../../third-party/nanors/deps/obl/oblas_lite.c"
size_t nanors_test_alignment(unsigned features) {
    test_features = features;
    struct oblas_impl selected;
    nanors_test_get_impl(&selected);
    return selected.align_size;
}
#endif
