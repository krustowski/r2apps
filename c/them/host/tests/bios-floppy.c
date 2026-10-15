#include "int.h"
#include "printf.h"

uint8_t debug = 0;
static Memory_T ram;

#define CHECK(expr) do { if (!(expr)) { \
    printf((const uint8_t *)"BIOS floppy regression FAIL at line %u\n", (unsigned int)__LINE__); \
    return 1; } } while (0)

static void request(CPU_T *cpu, uint16_t ax, uint16_t cx, uint16_t dx) {
    cpu->AX = ax;
    cpu->CX = cx;
    cpu->DX = dx;
    handle_13h(cpu, &ram);
}

int main(void) {
    CPU_T cpu = {0};
    init_ram(&ram);
    bios_disk_init(&ram);

    cpu.FLAGS = 0x0203;
    request(&cpu, 0x00AB, 0, 0);
    CHECK(!get_flag(&cpu, CF) && cpu.AX == 0x00AB);
    request(&cpu, 0, 0, 1);
    CHECK(get_flag(&cpu, CF) && (cpu.AX >> 8) == 0x80);
    CHECK(mem_r8(&ram, 0x40, 0x41) == 0x80);
    request(&cpu, 0x0100, 0, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x8000);
    request(&cpu, 0, 0, 0);
    CHECK(!get_flag(&cpu, CF) && mem_r8(&ram, 0x40, 0x41) == 0);

    request(&cpu, 0x0800, 0, 0);
    CHECK(!get_flag(&cpu, CF) && cpu.AX == 0 && cpu.BX == 4);
    CHECK(cpu.CX == 0x4F12 && cpu.DX == 0x0101);
    CHECK(mem_r16(&ram, 0, 0x78) == cpu.DI && mem_r16(&ram, 0, 0x7A) == cpu.ES);
    CHECK(mem_r8(&ram, cpu.ES, (uint16_t)(cpu.DI + 3)) == 2);
    CHECK(mem_r8(&ram, cpu.ES, (uint16_t)(cpu.DI + 4)) == 18);

    cpu.ES = 0x2000;
    cpu.BX = 0x0200;
    mem_w8(&ram, cpu.ES, 0x01FF, 0xA5);
    mem_w8(&ram, cpu.ES, 0x0400, 0xA5);
    request(&cpu, 0x0201, 1, 0);
    CHECK(!get_flag(&cpu, CF) && cpu.AX == 1);
    CHECK(mem_r16(&ram, cpu.ES, 0x020B) == 512);
    CHECK(mem_r16(&ram, cpu.ES, 0x0213) == 2880);
    CHECK(mem_r16(&ram, cpu.ES, 0x0216) == 9);
    CHECK(mem_r16(&ram, cpu.ES, 0x0218) == 18);
    CHECK(mem_r16(&ram, cpu.ES, 0x021A) == 2);
    CHECK(mem_r16(&ram, cpu.ES, 0x03FE) == 0xAA55);
    CHECK(mem_r8(&ram, cpu.ES, 0x01FF) == 0xA5 && mem_r8(&ram, cpu.ES, 0x0400) == 0xA5);

    request(&cpu, 0x020A, 2, 0); /* both FATs */
    CHECK(!get_flag(&cpu, CF) && cpu.AX == 10);
    CHECK(mem_r16(&ram, cpu.ES, cpu.BX) == 0xFFF0);
    CHECK(mem_r8(&ram, cpu.ES, (uint16_t)(cpu.BX + 2)) == 0xFF);
    CHECK(mem_r16(&ram, cpu.ES, (uint16_t)(cpu.BX + 9 * 512)) == 0xFFF0);
    request(&cpu, 0x0202, 18, 0); /* cross from head 0 to head 1 */
    CHECK(!get_flag(&cpu, CF));
    for (uint16_t i = 0; i < 1024; i++)
        CHECK(mem_r8(&ram, cpu.ES, (uint16_t)(cpu.BX + i)) == 0);

    request(&cpu, 0x0200, 1, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0201, 0, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0201, 19, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0201, 1, 0x0200);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0201, 0x5001, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0201, 0x0041, 0); /* cylinder high bits */
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);
    request(&cpu, 0x0202, 0x4F12, 0x0100); /* past end of medium */
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0400);

    cpu.BX = 0xFE00;
    request(&cpu, 0x0201, 1, 0);
    CHECK(!get_flag(&cpu, CF));
    ram.bytes[0x30000] = 0xA5;
    request(&cpu, 0x0202, 1, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0900 && ram.bytes[0x30000] == 0xA5);
    cpu.ES = 0x1FF0;
    cpu.BX = 0;
    request(&cpu, 0x0201, 1, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0900);
    request(&cpu, 0x0401, 1, 0); /* verify does not use the DMA buffer */
    CHECK(!get_flag(&cpu, CF));
    request(&cpu, 0x0301, 1, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0300);
    request(&cpu, 0x0100, 0, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0300);
    request(&cpu, 0xFF00, 0, 0);
    CHECK(get_flag(&cpu, CF) && cpu.AX == 0x0100);
    request(&cpu, 0x1500, 0, 0);
    CHECK(!get_flag(&cpu, CF) && cpu.AX == 0x0100);
    printf((const uint8_t *)"BIOS floppy regression PASS\n");
    return 0;
}
