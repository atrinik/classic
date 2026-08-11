#include <toolkit/math.h>

int main(void) {
    toolkit_import(math);

    int expected[4];
    rndm_seed(UINT64_C(42));
    for (size_t i = 0; i < arraysize(expected); i++) {
        expected[i] = rndm(-100, 100);
    }

    rndm_seed(UINT64_C(42));
    for (size_t i = 0; i < arraysize(expected); i++) {
        if (rndm(-100, 100) != expected[i]) {
            toolkit_deinit();
            return 1;
        }
    }

    toolkit_deinit();
    return 0;
}
