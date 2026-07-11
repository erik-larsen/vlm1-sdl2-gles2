/* Test tool: decompress all banks and write the raw matrix images to
 * stdout (10 banks x 55296 bytes), for diffing against the field dumps
 * in the reference banks.s comments (scripts/validate_banks.py). */
#include <stdio.h>

#include "../src/banks.h"
#include "../src/banks_data.h"

int main(void)
{
    static uint8_t matrix[VLM_MATRIX_SIZE];
    for (int bank = 0; bank < VLM_NBANKS; bank++) {
        vlm_load_bank(bank, matrix);
        if (fwrite(matrix, 1, sizeof matrix, stdout) != sizeof matrix) {
            perror("fwrite");
            return 1;
        }
    }
    return 0;
}
