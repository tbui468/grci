#include "grci.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

//can we access nested submodules
//Are these microops enough for all 5 instructions (HLT, LDA, ADD, SUB, STA)?
//Need to fit all microops into 8 bits to simplify computer
//  MAR dst
//  MDR dst
//  MDR src
//  Operand src
//  PC src
//  ALU src
//  negY
//  halt
//Get remaining instructions working (ADD, SUB, STA)
//remove NOP and HLT from possible instructions that create microops so that a single Mux4Way8 can be used
//  will also need to offset machine code so LDA starts at 0000 rather than 1000
//make all control signals return a noop if ring counter not in t3, t4, or t5

//Redo instructions so that NOOP is no longer an instruction
//  LDA, ADD, SUB, STA, HLT

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
    if (!grci_compile_src(g, buf, size)) {
        printf("%s\n", grci_err());
    }

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
                        0, 1, 0, 0, 0, 1, 1, 1,
                        1, 1, 0, 0, 1, 0, 1, 1,
                        0, 0, 1, 0, 0, 0, 1, 1,

                        1, 0, 1, 0, 0, 0, 0, 0,
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

    struct grci_submodule *mar = grci_submodule(module, "mar", 3);
    //struct grci_submodule *cir = grci_submodule(module, "cir", 3);

    struct grci_submodule *a = grci_submodule(module, "acc", 3);
    struct grci_submodule *b = grci_submodule(module, "mdr", 3);
    struct grci_submodule *pc = grci_submodule(module, "pc", 2);
    struct grci_submodule *cu = grci_submodule(module, "cu", 2);

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

            printf("%3s MAR:", "");
            for (int i = 0; i < mar->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", mar->states[i]);
            }
            /*
            printf("%3s CIR:", "");
            for (int i = 0; i < cir->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", cir->states[i]);
            }
            */
            printf("\n    ACC:");
            for (int i = 0; i < a->state_count; i++) {
                if (i % 8 == 0) printf(" ");
                printf("%d", a->states[i]);
            }
            printf("%3s    MDR:", "");
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
