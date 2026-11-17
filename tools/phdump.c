#include <stdio.h>
#include <string.h>
#include "bst.h"
#include "bst_text.h"
#include "bst_token.h"
#include "bst_priv.h"
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: phdump BUILD < lines\n");
        return 2;
    }
    bst *h = bst_open(argv[1]);
    if (!h)
        return 1;
    const bst_image *img = bst_handle_image(h);
    char line[512];
    static uint8_t stream[0x400];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        bst_tok tk; bst_assembler z; uint8_t buf[128];
        memset(stream, 0, sizeof stream);
        bst_tok_init(&tk, img, line);
        bst_assemble_init(&z, img, stream);
        bst_assemble_start(&z);
        printf("%s\t", line);
        for (int g = 0; g < 256; g++) {
            memset(buf, 0, sizeof buf);
            int kind = bst_tok_next(&tk, buf);
            z.mode = 0;
            if (!bst_assemble_token(&z, kind, buf)) { if (kind == 6) break; continue; }
            int len = z.len > 0 ? z.len : z.wp + 1;
            /* The French rules drop the liaison consonants no word wants. */
            if (len > 0)
                bst_phrules(img, stream, &len, (int)sizeof stream, 0);
            for (int i = 0; i < len; i++) if (bst_is_sound(stream[i])) printf("%02x ", stream[i]);
            printf("| ");
            memset(stream, 0, sizeof stream);
            bst_assemble_start(&z);
            if (kind == 6) break;
        }
        printf("\n");
    }
}
