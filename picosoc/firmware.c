// --------------------------------------------------------
// uart_printf: minimal printf for PicoSoC firmware
//
// Paste into firmware.c after putchar(). Needs no libc and no libgcc:
// decimal conversion uses repeated subtraction, so it works with
// -nostdlib and -march=rv32ic (no mul/div instructions).
//
// Supported: %d %i %u %x %X %c %s %%
//            optional '0' flag and field width (e.g. %08x, %5d)
//            'l' length modifier is accepted and ignored (32-bit only)
//
// Written for this project. Design references:
//   - PicoRV32 dhrystone/stdlib.c (printf on top of a putchar)
//       https://github.com/YosysHQ/picorv32
//   - PicoSoC firmware.c print_dec (division-free decimal output)
//       https://github.com/YosysHQ/picorv32/tree/main/picosoc
//   - mpaland/printf (format flag / width handling)
//       https://github.com/mpaland/printf
// --------------------------------------------------------
 


#define PRINTF
#include <stdint.h>
#include <stdbool.h>

#include <stdarg.h> // add for printf


#define reg_leds (*(volatile uint32_t*)0x03000000)
#define reg_uart_clkdiv (*(volatile uint32_t*)0x02000004)
#define reg_uart_data (*(volatile uint32_t*)0x02000008)



#define CPU_FREQ_HZ 12000000


// spi io 


#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000 /* 128 KB */
#elif HX8KDEMO
#  define MEM_TOTAL 0x200 /* 2 KB */
#else
#  error "Set -DICEBREAKER or -DHX8KDEMO when compiling firmware.c"
#endif

// a pointer to this is a null pointer, but the compiler does not
// know that because "sram" is a linker symbol from sections.lds.
extern uint32_t sram;

#define reg_spictrl (*(volatile uint32_t*)0x02000000)

#ifdef PRINTF
void putchar(char c);
static void printf_emit(const char *buf, int len, int neg, int width, char pad);
static void printf_dec(uint32_t val, int neg, int width, char pad);
static void printf_hex(uint32_t val, int upper, int width, char pad);
void uart_printf(const char *fmt, ...);


static void printf_emit(const char *buf, int len, int neg, int width, char pad)
{
	int total = len + (neg ? 1 : 0);
 
	if (pad == '0') {
		if (neg) putchar('-');
		for (int i = total; i < width; i++) putchar('0');
	} else {
		for (int i = total; i < width; i++) putchar(' ');
		if (neg) putchar('-');
	}
	for (int i = 0; i < len; i++) putchar(buf[i]);
}
static void printf_dec(uint32_t val, int neg, int width, char pad)
{
	static const uint32_t pow10[10] = {
		1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
		10000u, 1000u, 100u, 10u, 1u
	};
	char buf[10];
	int len = 0;
 
	for (int i = 0; i < 10; i++) {
		char d = '0';
		while (val >= pow10[i]) {
			val -= pow10[i];
			d++;
		}
		if (d != '0' || len > 0 || i == 9)
			buf[len++] = d;
	}
	printf_emit(buf, len, neg, width, pad);
}
static void printf_hex(uint32_t val, int upper, int width, char pad)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char buf[8];
	int len = 0;
 
	for (int shift = 28; shift >= 0; shift -= 4) {
		int nib = (val >> shift) & 0xF;
		if (nib != 0 || len > 0 || shift == 0)
			buf[len++] = digits[nib];
	}
	printf_emit(buf, len, 0, width, pad);
}
 
void uart_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
 
	while (*fmt) {
		if (*fmt != '%') {
			putchar(*fmt++);
			continue;
		}
		fmt++;
 
		char pad = ' ';
		int width = 0;
 
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			// width * 10 without a multiply instruction
			width = (width << 3) + (width << 1) + (*fmt - '0');
			fmt++;
		}
		while (*fmt == 'l')
			fmt++;
 
		switch (*fmt) {
		case 'd':
		case 'i': {
			int32_t v = va_arg(ap, int32_t);
			if (v < 0)
				printf_dec(0u - (uint32_t)v, 1, width, pad);
			else
				printf_dec((uint32_t)v, 0, width, pad);
			break;
		}
		case 'u':
			printf_dec(va_arg(ap, uint32_t), 0, width, pad);
			break;
		case 'x':
			printf_hex(va_arg(ap, uint32_t), 0, width, pad);
			break;
		case 'X':
			printf_hex(va_arg(ap, uint32_t), 1, width, pad);
			break;
		case 'c':
			putchar((char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len = 0;
			if (!s) s = "(null)";
			while (s[len]) len++;
			for (int i = len; i < width; i++) putchar(' ');
			while (*s) putchar(*s++);
			break;
		}
		case '%':
			putchar('%');
			break;
		case 0:
			va_end(ap);
			return;
		default:
			putchar('%');
			putchar(*fmt);
			break;
		}
		fmt++;
	}
 
	va_end(ap);
}

#endif


extern uint32_t flashio_worker_begin;
extern uint32_t flashio_worker_end;

void putchar(char c);
void print(const char *p);
void print_hex(uint32_t v, int digits);
void print_dec(uint32_t v);
char getchar_prompt(char *prompt);


void flashio(uint8_t *data, int len, uint8_t wrencmd)
{
	uint32_t func[&flashio_worker_end - &flashio_worker_begin];

	uint32_t *src_ptr = &flashio_worker_begin;
	uint32_t *dst_ptr = func;

	while (src_ptr != &flashio_worker_end)
		*(dst_ptr++) = *(src_ptr++);

	((void(*)(uint8_t*, uint32_t, uint32_t))func)(data, len, wrencmd);
}

#ifdef HX8KDEMO
void set_flash_qspi_flag()
{
	uint8_t buffer[8];
	uint32_t addr_cr1v = 0x800002;

	// Read Any Register (RDAR 65h)
	buffer[0] = 0x65;
	buffer[1] = addr_cr1v >> 16;
	buffer[2] = addr_cr1v >> 8;
	buffer[3] = addr_cr1v;
	buffer[4] = 0; // dummy
	buffer[5] = 0; // rdata
	flashio(buffer, 6, 0);
	uint8_t cr1v = buffer[5];

	// Write Enable (WREN 06h) + Write Any Register (WRAR 71h)
	buffer[0] = 0x71;
	buffer[1] = addr_cr1v >> 16;
	buffer[2] = addr_cr1v >> 8;
	buffer[3] = addr_cr1v;
	buffer[4] = cr1v | 2; // Enable QSPI
	flashio(buffer, 5, 0x06);
}

void set_flash_latency(uint8_t value)
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | ((value & 15) << 16);

	uint32_t addr = 0x800004;
	uint8_t buffer_wr[5] = {0x71, addr >> 16, addr >> 8, addr, 0x70 | value};
	flashio(buffer_wr, 5, 0x06);
}

void set_flash_mode_spi()
{
	reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00000000;
}

void set_flash_mode_dual()
{
	reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00400000;
}

void set_flash_mode_quad()
{
	reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00200000;
}

void set_flash_mode_qddr()
{
	reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00600000;
}
#endif

#ifdef ICEBREAKER
void set_flash_qspi_flag()
{
	uint8_t buffer[8];

	// Read Configuration Registers (RDCR1 35h)
	buffer[0] = 0x35;
	buffer[1] = 0x00; // rdata
	flashio(buffer, 2, 0);
	uint8_t sr2 = buffer[1];

	// Write Enable Volatile (50h) + Write Status Register 2 (31h)
	buffer[0] = 0x31;
	buffer[1] = sr2 | 2; // Enable QSPI
	flashio(buffer, 2, 0x50);
}

void set_flash_mode_spi()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00000000;
}

void set_flash_mode_dual()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00400000;
}

void set_flash_mode_quad()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00240000;
}

void set_flash_mode_qddr()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00670000;
}

void enable_flash_crm()
{
	reg_spictrl |= 0x00100000;
}
#endif

// --------------------------------------------------------




void print_dec(uint32_t v)
{
	if (v >= 1000) {
		print(">=1000");
		return;
	}

	if      (v >= 900) { putchar('9'); v -= 900; }
	else if (v >= 800) { putchar('8'); v -= 800; }
	else if (v >= 700) { putchar('7'); v -= 700; }
	else if (v >= 600) { putchar('6'); v -= 600; }
	else if (v >= 500) { putchar('5'); v -= 500; }
	else if (v >= 400) { putchar('4'); v -= 400; }
	else if (v >= 300) { putchar('3'); v -= 300; }
	else if (v >= 200) { putchar('2'); v -= 200; }
	else if (v >= 100) { putchar('1'); v -= 100; }

	if      (v >= 90) { putchar('9'); v -= 90; }
	else if (v >= 80) { putchar('8'); v -= 80; }
	else if (v >= 70) { putchar('7'); v -= 70; }
	else if (v >= 60) { putchar('6'); v -= 60; }
	else if (v >= 50) { putchar('5'); v -= 50; }
	else if (v >= 40) { putchar('4'); v -= 40; }
	else if (v >= 30) { putchar('3'); v -= 30; }
	else if (v >= 20) { putchar('2'); v -= 20; }
	else if (v >= 10) { putchar('1'); v -= 10; }

	if      (v >= 9) { putchar('9'); v -= 9; }
	else if (v >= 8) { putchar('8'); v -= 8; }
	else if (v >= 7) { putchar('7'); v -= 7; }
	else if (v >= 6) { putchar('6'); v -= 6; }
	else if (v >= 5) { putchar('5'); v -= 5; }
	else if (v >= 4) { putchar('4'); v -= 4; }
	else if (v >= 3) { putchar('3'); v -= 3; }
	else if (v >= 2) { putchar('2'); v -= 2; }
	else if (v >= 1) { putchar('1'); v -= 1; }
	else putchar('0');
}


char getchar()
{
	return getchar_prompt(0);
}

void cmd_print_spi_state()
{
	print("SPI State:\n");

	print("  LATENCY ");
	print_dec((reg_spictrl >> 16) & 15);
	print("\n");

	print("  DDR ");
	if ((reg_spictrl & (1 << 22)) != 0)
		print("ON\n");
	else
		print("OFF\n");

	print("  QSPI ");
	if ((reg_spictrl & (1 << 21)) != 0)
		print("ON\n");
	else
		print("OFF\n");

	print("  CRM ");
	if ((reg_spictrl & (1 << 20)) != 0)
		print("ON\n");
	else
		print("OFF\n");
}

uint32_t xorshift32(uint32_t *state)
{
	/* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;

	return x;
}

void cmd_memtest()
{
	int cyc_count = 5;
	int stride = 256;
	uint32_t state;

	volatile uint32_t *base_word = (uint32_t *) 0;
	volatile uint8_t *base_byte = (uint8_t *) 0;

	print("Running memtest ");

	// Walk in stride increments, word access
	for (int i = 1; i <= cyc_count; i++) {
		state = i;

		for (int word = 0; word < MEM_TOTAL / sizeof(int); word += stride) {
			*(base_word + word) = xorshift32(&state);
		}

		state = i;

		for (int word = 0; word < MEM_TOTAL / sizeof(int); word += stride) {
			if (*(base_word + word) != xorshift32(&state)) {
				print(" ***FAILED WORD*** at ");
				print_hex(4*word, 4);
				print("\n");
				return;
			}
		}

		print(".");
	}

	// Byte access
	for (int byte = 0; byte < 128; byte++) {
		*(base_byte + byte) = (uint8_t) byte;
	}

	for (int byte = 0; byte < 128; byte++) {
		if (*(base_byte + byte) != (uint8_t) byte) {
			print(" ***FAILED BYTE*** at ");
			print_hex(byte, 4);
			print("\n");
			return;
		}
	}

	print(" passed\n");
}

// --------------------------------------------------------

void cmd_read_flash_id()
{
	uint8_t buffer[17] = { 0x9F, /* zeros */ };
	flashio(buffer, 17, 0);

	for (int i = 1; i <= 16; i++) {
		putchar(' ');
		print_hex(buffer[i], 2);
	}
	putchar('\n');
}

// --------------------------------------------------------

#ifdef HX8KDEMO
uint8_t cmd_read_flash_regs_print(uint32_t addr, const char *name)
{
	set_flash_latency(8);

	uint8_t buffer[6] = {0x65, addr >> 16, addr >> 8, addr, 0, 0};
	flashio(buffer, 6, 0);

	print("0x");
	print_hex(addr, 6);
	print(" ");
	print(name);
	print(" 0x");
	print_hex(buffer[5], 2);
	print("\n");

	return buffer[5];
}

void cmd_read_flash_regs()
{
	print("\n");
	uint8_t sr1v = cmd_read_flash_regs_print(0x800000, "SR1V");
	uint8_t sr2v = cmd_read_flash_regs_print(0x800001, "SR2V");
	uint8_t cr1v = cmd_read_flash_regs_print(0x800002, "CR1V");
	uint8_t cr2v = cmd_read_flash_regs_print(0x800003, "CR2V");
	uint8_t cr3v = cmd_read_flash_regs_print(0x800004, "CR3V");
	uint8_t vdlp = cmd_read_flash_regs_print(0x800005, "VDLP");
}
#endif

#ifdef ICEBREAKER
uint8_t cmd_read_flash_reg(uint8_t cmd)
{
	uint8_t buffer[2] = {cmd, 0};
	flashio(buffer, 2, 0);
	return buffer[1];
}

void print_reg_bit(int val, const char *name)
{
	for (int i = 0; i < 12; i++) {
		if (*name == 0)
			putchar(' ');
		else
			putchar(*(name++));
	}

	putchar(val ? '1' : '0');
	putchar('\n');
}

void cmd_read_flash_regs()
{
	putchar('\n');

	uint8_t sr1 = cmd_read_flash_reg(0x05);
	uint8_t sr2 = cmd_read_flash_reg(0x35);
	uint8_t sr3 = cmd_read_flash_reg(0x15);

	print_reg_bit(sr1 & 0x01, "S0  (BUSY)");
	print_reg_bit(sr1 & 0x02, "S1  (WEL)");
	print_reg_bit(sr1 & 0x04, "S2  (BP0)");
	print_reg_bit(sr1 & 0x08, "S3  (BP1)");
	print_reg_bit(sr1 & 0x10, "S4  (BP2)");
	print_reg_bit(sr1 & 0x20, "S5  (TB)");
	print_reg_bit(sr1 & 0x40, "S6  (SEC)");
	print_reg_bit(sr1 & 0x80, "S7  (SRP)");
	putchar('\n');

	print_reg_bit(sr2 & 0x01, "S8  (SRL)");
	print_reg_bit(sr2 & 0x02, "S9  (QE)");
	print_reg_bit(sr2 & 0x04, "S10 ----");
	print_reg_bit(sr2 & 0x08, "S11 (LB1)");
	print_reg_bit(sr2 & 0x10, "S12 (LB2)");
	print_reg_bit(sr2 & 0x20, "S13 (LB3)");
	print_reg_bit(sr2 & 0x40, "S14 (CMP)");
	print_reg_bit(sr2 & 0x80, "S15 (SUS)");
	putchar('\n');

	print_reg_bit(sr3 & 0x01, "S16 ----");
	print_reg_bit(sr3 & 0x02, "S17 ----");
	print_reg_bit(sr3 & 0x04, "S18 (WPS)");
	print_reg_bit(sr3 & 0x08, "S19 ----");
	print_reg_bit(sr3 & 0x10, "S20 ----");
	print_reg_bit(sr3 & 0x20, "S21 (DRV0)");
	print_reg_bit(sr3 & 0x40, "S22 (DRV1)");
	print_reg_bit(sr3 & 0x80, "S23 (HOLD)");
	putchar('\n');
}
#endif

// --------------------------------------------------------

uint32_t cmd_benchmark(bool verbose, uint32_t *instns_p)
{
	uint8_t data[256];
	uint32_t *words = (void*)data;

	uint32_t x32 = 314159265;

	uint32_t cycles_begin, cycles_end;
	uint32_t instns_begin, instns_end;
	__asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_begin));

	for (int i = 0; i < 20; i++)
	{
		for (int k = 0; k < 256; k++)
		{
			x32 ^= x32 << 13;
			x32 ^= x32 >> 17;
			x32 ^= x32 << 5;
			data[k] = x32;
		}

		for (int k = 0, p = 0; k < 256; k++)
		{
			if (data[k])
				data[p++] = k;
		}

		for (int k = 0, p = 0; k < 64; k++)
		{
			x32 = x32 ^ words[k];
		}
	}

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_end));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_end));

	if (verbose)
	{
		print("Cycles: 0x");
		print_hex(cycles_end - cycles_begin, 8);
		putchar('\n');

		print("Instns: 0x");
		print_hex(instns_end - instns_begin, 8);
		putchar('\n');

		print("Chksum: 0x");
		print_hex(x32, 8);
		putchar('\n');
	}

	if (instns_p)
		*instns_p = instns_end - instns_begin;

	return cycles_end - cycles_begin;
}

// --------------------------------------------------------

#ifdef HX8KDEMO
void cmd_benchmark_all()
{
	uint32_t instns = 0;

	print("default        ");
	reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00000000;
	print(": ");
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	for (int i = 8; i > 0; i--)
	{
		print("dspi-");
		print_dec(i);
		print("         ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00400000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	for (int i = 8; i > 0; i--)
	{
		print("dspi-crm-");
		print_dec(i);
		print("     ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00500000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	for (int i = 8; i > 0; i--)
	{
		print("qspi-");
		print_dec(i);
		print("         ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00200000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	for (int i = 8; i > 0; i--)
	{
		print("qspi-crm-");
		print_dec(i);
		print("     ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00300000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	for (int i = 8; i > 0; i--)
	{
		print("qspi-ddr-");
		print_dec(i);
		print("     ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00600000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	for (int i = 8; i > 0; i--)
	{
		print("qspi-ddr-crm-");
		print_dec(i);
		print(" ");

		set_flash_latency(i);
		reg_spictrl = (reg_spictrl & ~0x00700000) | 0x00700000;

		print(": ");
		print_hex(cmd_benchmark(false, &instns), 8);
		putchar('\n');
	}

	print("instns         : ");
	print_hex(instns, 8);
	putchar('\n');
}
#endif

#ifdef ICEBREAKER
void cmd_benchmark_all()
{
	uint32_t instns = 0;

	print("default   ");
	set_flash_mode_spi();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	print("dual      ");
	set_flash_mode_dual();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	// print("dual-crm  ");
	// enable_flash_crm();
	// print_hex(cmd_benchmark(false, &instns), 8);
	// putchar('\n');

	print("quad      ");
	set_flash_mode_quad();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	print("quad-crm  ");
	enable_flash_crm();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	print("qddr      ");
	set_flash_mode_qddr();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

	print("qddr-crm  ");
	enable_flash_crm();
	print_hex(cmd_benchmark(false, &instns), 8);
	putchar('\n');

}
#endif

void cmd_echo()
{
	print("Return to menu by sending '!'\n\n");
	char c;
	while ((c = getchar()) != '!')
		putchar(c);
}

//////////////////////end of demo funcs//////////////////////////


static void delay_ms(uint32_t ms)
{
    uint32_t start;
    uint32_t now;
    uint32_t cycles = (CPU_FREQ_HZ / 1000) * ms;

    __asm__ volatile ("rdcycle %0" : "=r"(start));

    do {
        __asm__ volatile ("rdcycle %0" : "=r"(now));
    } while ((now - start) < cycles);
}


void putchar(char c)
{
        if (c == '\n')
                putchar('\r');
        reg_uart_data = c;
}

void print(const char *p)
{
        while (*p)
                putchar(*(p++));
}

void print_hex(uint32_t v, int digits)
{
        for (int i = 7; i >= 0; i--) {
                char c = "0123456789abcdef"[(v >> (4*i)) & 15];
                if (c == '0' && i >= digits) continue;
                putchar(c);
                digits = i;
        }
}


char getchar_prompt(char *prompt)
{
        int32_t c = -1;

        uint32_t cycles_begin, cycles_now, cycles;
        __asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));

        reg_leds = ~0;

        if (prompt)
                print(prompt);

        while (c == -1) {
                __asm__ volatile ("rdcycle %0" : "=r"(cycles_now));
                cycles = cycles_now - cycles_begin;
                if (cycles > 12000000) {
                        if (prompt)
                                print(prompt);
                        cycles_begin = cycles_now;
                        reg_leds = ~reg_leds;
                }
                c = reg_uart_data;
        }

        reg_leds = 0;
        return c;
}

void cmd_printf()
{
    uart_printf("hello picorv32 world!\n");
    uart_printf("dec=%d neg=%d hex=0x%x str=%s chr=%c\n", 12345, -42, 0xbeef, "pico", 'A');
}

#include "shell.c"
int main(void)
{

        reg_leds = 31;
        reg_uart_clkdiv = 104;
        print("Booting..\n");

        reg_leds = 63;
        //set_flash_qspi_flag();

        reg_leds = 127;
        while (getchar_prompt("Press - ENTER to continue......\n") != '\r') { /* wait */ }

        print("\n");
        print("  ____  _          ____         ____\n");
        print(" |  _ \\(_) ___ ___/ ___|  ___  / ___|\n");
        print(" | |_) | |/ __/ _ \\___ \\ / _ \\| |\n");
        print(" |  __/| | (_| (_) |__) | (_) | |___\n");
        print(" |_|   |_|\\___\\___/____/ \\___/ \\____|\n");
        print("\n");





	//while (1)
        {
	    print("test print\n");
	    delay_ms(500);

	}

	print("Total memory: ");
	print_dec(MEM_TOTAL / 1024);
	print(" KiB\n");
	print("\n");

	//cmd_memtest(); // test overwrites bss and data memory
	print("\n");

	cmd_print_spi_state();
	print("\n");

	while (1)
	{
		print("\n");

		print("Select an action:\n");
		print("\n");
		//print("   [1] Read SPI Flash ID\n");
		//print("   [2] Read SPI Config Regs\n");
		//print("   [3] Switch to default mode\n");
		//print("   [4] Switch to Dual I/O mode\n");
		//print("   [5] Switch to Quad I/O mode\n");
		//print("   [6] Switch to Quad DDR mode\n");
		//print("   [7] Toggle continuous read mode\n");
		print("   [9] Run simplistic benchmark\n");
		print("   [0] Benchmark all configs\n");
		print("   [M] Run Memtest\n");
		print("   [S] Print SPI state\n");
		print("   [e] Echo UART\n");
		print("   [f] printf function test \n");
                print("   [c] Command shell\n");
		print("\n");

		for (int rep = 10; rep > 0; rep--)
		{
			print("Command> ");
			char cmd = getchar();
			if (cmd > 32 && cmd < 127)
				putchar(cmd);
			print("\n");

			switch (cmd)
			{
			case '1':
				cmd_read_flash_id();
				break;
			case '2':
				cmd_read_flash_regs();
				break;
			case '3':
				set_flash_mode_spi();
				break;
			case '4':
				set_flash_mode_dual();
				break;
			case '5':
				set_flash_mode_quad();
				break;
			case '6':
				set_flash_mode_qddr();
				break;
			case '7':
				reg_spictrl = reg_spictrl ^ 0x00100000;
				break;
			case '9':
				cmd_benchmark(true, 0);
				break;
			case '0':
				cmd_benchmark_all();
				break;
			case 'M':
				cmd_memtest();
				break;
			case 'S':
				cmd_print_spi_state();
				break;
			case 'e':
				cmd_echo();
				break;
			case 'f':
				cmd_printf();
				break;
			case 'c':
				shell_run();
				break;
			default:
				continue;
			}

			break;
		}
	}

		 



    return 0;
}
