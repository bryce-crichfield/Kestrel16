/* Shifts and stores: prints a checksum the emulator and the gate-level computer must agree on. */
static void putc_(int c) { while (!(*(volatile unsigned *)0xFC82 & 2)) {} *(volatile unsigned *)0xFC80 = c; }
static void hex(unsigned v) { int i; for (i = 12; i >= 0; i -= 4) putc_("0123456789ABCDEF"[(v >> i) & 15]); putc_(' '); }

unsigned char bytes[8];
unsigned words[4];

int main(void)
{
    unsigned values[3] = {0x8001, 0x7FFE, 0xA5C3};
    unsigned sum = 0, k;
    int i, n;
    for (i = 0; i < 3; i++)
        for (n = 0; n < 16; n++) {
            unsigned v = values[i];
            int s = (int)v;
            sum = sum * 31 + (v << n);
            sum = sum * 31 + (v >> n);
            sum = sum * 31 + (unsigned)(s >> n);
        }
    hex(sum);
    for (k = 0; k < 8; k++) bytes[k] = (unsigned char)(k * 37 + 1);
    for (k = 0; k < 4; k++) words[k] = k * 4097 + 3;
    sum = 0;
    for (k = 0; k < 8; k++) sum = sum * 7 + bytes[k];
    for (k = 0; k < 4; k++) sum = sum * 7 + words[k];
    hex(sum);
    putc_('\n');
    return 0;
}
