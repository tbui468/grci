#include "grci.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

//make all control signals return a noop if ring counter not in t3, t4, or t5
//make the halt instruction clear the ring counter (set everything to 0).
//this will make instructions do nothing - can just have computer run for 16 instruction cycles and then end
//make program counter only have reset and remove in[8] and load

//remove register B - loading should go the MDR and an extra cycle in fetch will be used to load CIR with MDR
//rename register A to ACC
//rename register INSTR to CIR

static char* read_file(const char* path, size_t *size) {
    FILE *file = fopen(path, "rb");
    fseek(file, 0L, SEEK_END);
    size_t s = ftell(file);
    rewind(file);
    char *buf = malloc(s + 1);
    size_t read = fread(buf, sizeof(char), s, file);
    buf[read] = '\0';
    fclose(file);
    *size = s;
    return buf;
}


int main(int argc, char **argv) {
    size_t size;
    char *buf = read_file("modules.hdl", &size);
    struct grci *g = grci_init(malloc, realloc, free);
    grci_compile_src(g, buf, size);

    const char *not_module = "Computer";
    struct grci_module *module = grci_init_module(g, not_module, strlen(not_module));

/*
    NOP 0000 XXXX
    LDA 1000 AAAA
    ADD 0100 AAAA
    SUB 1100 AAAA
    STA 0010 AAAA
    HLT 1010 XXXX
*/

    const bool rom[] = {1, 0, 0, 0, 1, 1, 1, 1,
                        1, 0, 1, 0, 0, 0, 0, 0,

                        0, 1, 0, 0, 0, 1, 1, 1,
                        1, 1, 0, 0, 1, 0, 1, 1,
                        0, 0, 1, 0, 0, 0, 1, 1,

                        //1, 0, 1, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,

                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,

                        0, 0, 0, 0, 0, 0, 0, 0,
                        1, 0, 0, 0, 0, 0, 0, 0,
                        0, 1, 0, 0, 0, 0, 0, 0,
                        1, 1, 0, 0, 0, 0, 0, 0};
     

    struct grci_submodule *ram = grci_submodule(module, "ram", 3);
    memcpy(ram->states, rom, sizeof(rom));

    struct grci_submodule *out = grci_submodule(module, "out", 3);
    struct grci_submodule *mar = grci_submodule(module, "mar", 3);
    struct grci_submodule *ins = grci_submodule(module, "cir", 3);

    struct grci_submodule *a = grci_submodule(module, "acc", 3);
    struct grci_submodule *b = grci_submodule(module, "mdr", 3);
    struct grci_submodule *pc = grci_submodule(module, "pc", 2);

    int cycle = 0;
    module->inputs[0] = 1; //reset will run until after the first high clock signal

    bool run_until_end = false;

    while (true) {
        int clock = grci_step_module(module);
        if (clock) {
            cycle++;
            module->inputs[0] = 0; //set reset to 0 so that computer can start running after first high clock signal
        }

        //only printing if clock high and not loading RAM cycle in the first 16 cycles
        bool halt = false;
        if (clock) {
            if (!run_until_end) {
                char c = getchar();
                if (c == 'r') run_until_end = true;
            }
            //need to eval nodes since combinational nodes are updated above
            halt = module->outputs[0];
            printf("Cycle: %d", cycle - 1);

            for (int i = 0; i < ram->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                if (i % 32 == 0) printf("\n");
                if (i == 0) printf("  RAM: ");
                else if (i % 32 == 0) printf("       ");
                printf("%d", ram->states[i]);
            }
            printf("\n");
            
            printf("Ouput:");
            for (int i = 0; i < out->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", out->states[i]);
            }

            printf("%3s  MAR:", "");
            for (int i = 0; i < mar->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", mar->states[i]);
            }
            printf("%3sInstr:", "");
            for (int i = 0; i < ins->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", ins->states[i]);
            }
            printf("\n    A:");
            for (int i = 0; i < a->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", a->states[i]);
            }
            printf("%3s    B:", "");
            for (int i = 0; i < b->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", b->states[i]);
            }
            printf("%3s   PC:", "");
            for (int i = 0; i < pc->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", pc->states[i]);
            }

            printf("\n\n");

        }


        if (halt) break;
    }



    free(buf);
    grci_destroy_module(module);
    grci_cleanup(g);
}
