// --------------------------------------------------------
// pico_simfs.c : PC simulator for the PicoSoC shell and PFS (Pico Flash Slots)
//
// Runs the unmodified shell.c and picofs.c on a PC. The SPI flash is simulated in RAM,
// so nothing is ever written to a real flash chip.
//
// Build:   gcc -Wall -o pico_simfs pico_simfs.c    (shell.c and picofs.c in the same directory)
// Run:     ./pico_simfs             keeps the flash contents in pfs_flash.img
//          ./pico_simfs other.img   use another image file
//          ./pico_simfs -           no image file (contents lost on exit)
//
// Running it again with the same image file is the equivalent of a power
// cycle: the files saved earlier are still there.
//
// What is simulated
//   - the spimemio configuration register in manual mode, pin by pin:
//     CS, clock, IO0 (MOSI), IO1 (MISO)
//   - a flash chip behind those pins:
//       0x05 read status   (BUSY stays set for a number of polls)
//       0x06 write enable  (required before every erase/program, then cleared)
//       0x20 sector erase  (4 KB, sets every byte to 0xFF)
//       0x02 page program  (can only clear bits, wraps inside a 256-byte page)
//       0x9f read JEDEC id
//   - memory-mapped flash reads (PORT_FLASH_BYTE)
//   - reg_uart_data / putchar on the terminal
//   - the CPU cycle counter (rdcycle), from the PC clock, for delay
//   - peek/poke: LED register 0x03000000 and UART divider 0x02000004;
//     every other address reads 0 and ignores writes
//
// The rule the board enforces, and the simulator checks
//   While the flash is busy, or while spimemio is in manual mode, the CPU
//   cannot fetch code from the flash. The simulator stops with a FATAL
//   message if the shell returns to memory-mapped mode while the flash is
//   still busy, or if flash-resident code (terminal I/O, flash reads) runs
//   while the flash is unreadable.
//
// What is NOT simulated
//   - the CPU itself, SPI timing, QSPI/CRM modes
//   - whether the RAM routines were really linked into RAM (check that on
//     the real build with:  nm icebreaker_fw.elf | grep flash_ram )
// --------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <poll.h>

#define SIM_BASE   0x00200000u          // must match PFS_BASE in picofs.c
#define SIM_SIZE   (8 * 4096)           // must match PFS_SLOTS * PFS_SECTOR
#define SIM_BUSY_ERASE   40             // status polls an erase stays busy
#define SIM_BUSY_PROGRAM 4              // status polls a page program stays busy

static uint8_t sim_flash[SIM_SIZE];
static const char *sim_image = "pfs_flash.img";
static int sim_erases, sim_programs;

static uint32_t sim_leds, sim_clkdiv = 104, sim_scratch;

static struct termios sim_tty_saved;
static int sim_tty_raw;

// pin and chip state
static int sim_memio = 1;               // 1 = memory-mapped mode
static int sim_cs = 1, sim_clk;
static int sim_miso;
static int sim_busy, sim_wel;
static uint8_t sim_shift, sim_cmd, sim_out;
static int sim_nbits, sim_nbytes, sim_outbits, sim_out_on, sim_out_idx;
static uint32_t sim_addr;
static uint8_t sim_data[1024];
static int sim_ndata;

// ---------------- terminal ----------------

static void sim_save_image(void)
{
	FILE *f;

	if (!sim_image)
		return;
	f = fopen(sim_image, "wb");
	if (!f) {
		fprintf(stderr, "[sim] cannot write %s\n", sim_image);
		return;
	}
	fwrite(sim_flash, 1, SIM_SIZE, f);
	fclose(f);
}

static void sim_cleanup(void)
{
	if (sim_tty_raw)
		tcsetattr(STDIN_FILENO, TCSANOW, &sim_tty_saved);
	sim_save_image();
	fprintf(stderr, "\n[sim] %d sector erases, %d page programs", sim_erases, sim_programs);
	if (sim_image)
		fprintf(stderr, ", image saved to %s", sim_image);
	fprintf(stderr, "\n");
}

static void sim_on_signal(int sig)
{
	exit(0);
}

static void sim_fatal(const char *what)
{
	fflush(stdout);
	fprintf(stderr, "\r\n[sim] FATAL: %s\r\n", what);
	fprintf(stderr, "[sim] On the board the CPU would fetch garbage from the flash here and hang.\r\n");
	exit(1);
}

// Called from everything that is flash-resident code on the board.
static void sim_need_flash(const char *who)
{
	static char msg[160];

	if (!sim_memio) {
		snprintf(msg, sizeof msg, "%s while spimemio is in manual mode", who);
		sim_fatal(msg);
	}
	if (sim_busy) {
		snprintf(msg, sizeof msg, "%s while the flash is still busy", who);
		sim_fatal(msg);
	}
}

static void sim_putchar(char c)
{
	sim_need_flash("terminal output");
	if (c == '\n')
		fputc('\r', stdout);        // same as putchar() in firmware.c
	fputc(c, stdout);
	fflush(stdout);
}

// Blocking, like the shell's wait loop on the board. End of input ends the simulator.
static int32_t sim_uart_read(void)
{
	unsigned char c;

	sim_need_flash("terminal input");
	if (read(STDIN_FILENO, &c, 1) != 1)
		exit(0);
	return c;
}

// The CPU cycle counter, derived from the PC clock at 12 MHz, so 'delay'
// takes the same time as on the board.
static uint32_t sim_cycles(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 12ull / 1000ull);
}

// 'reboot' restarts the firmware on the board. Here it ends the simulator;
// run it again to see the state after the restart.
static void sim_reboot(void)
{
	fprintf(stderr, "[sim] reboot: the firmware would restart here\r\n");
	exit(0);
}

// Non-blocking check for input, like reading -1 from the UART register on
// the board. A waiting byte is consumed.
static int sim_key_pressed(void)
{
	struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
	unsigned char c;

	sim_need_flash("terminal input");
	if (poll(&p, 1, 0) <= 0)
		return 0;
	if (read(STDIN_FILENO, &c, 1) != 1)
		exit(0);
	return 1;
}

// ---------------- memory-mapped registers (peek / poke / led) ----------------

static uint32_t *sim_mem(uint32_t addr)
{
	if (addr == 0x03000000u) return &sim_leds;
	if (addr == 0x02000004u) return &sim_clkdiv;
	sim_scratch = 0;
	return &sim_scratch;
}

// ---------------- flash chip model ----------------

static uint8_t sim_flash_byte(uint32_t addr)
{
	sim_need_flash("memory-mapped flash read");
	if (addr < SIM_BASE || addr >= SIM_BASE + SIM_SIZE) {
		fprintf(stderr, "[sim] read outside the PFS region: 0x%08x\n", addr);
		return 0xFF;
	}
	return sim_flash[addr - SIM_BASE];
}

static uint8_t sim_next_out(void)
{
	static const uint8_t id[3] = { 0xEF, 0x40, 0x18 };
	uint8_t v = 0xFF;

	if (sim_cmd == 0x05) {
		v = (sim_busy ? 0x01 : 0x00) | (sim_wel ? 0x02 : 0x00);
		if (sim_busy)
			sim_busy--;                     // time passes with every poll
	} else if (sim_cmd == 0x9f) {
		v = id[sim_out_idx < 3 ? sim_out_idx : 2];
	}
	sim_out_idx++;
	return v;
}

// One complete byte has been clocked into the chip.
static void sim_rx_byte(uint8_t b)
{
	if (sim_nbytes == 0) {
		sim_cmd = b;
		sim_addr = 0;
		sim_ndata = 0;
		if (b == 0x05 || b == 0x9f) {
			sim_out_on = 1;
			sim_out_idx = 0;
			sim_outbits = 0;
		}
	} else if (sim_nbytes <= 3) {
		sim_addr = (sim_addr << 8) | b;
	} else if (sim_ndata < (int)sizeof sim_data) {
		sim_data[sim_ndata++] = b;
	}
	sim_nbytes++;
}

// CS went high: the chip acts on what it received.
static void sim_end_command(void)
{
	if (sim_nbytes == 0 || sim_cmd == 0x05 || sim_cmd == 0x9f)
		return;
	if (sim_nbits != 0)
		fprintf(stderr, "[sim] command 0x%02x ended in the middle of a byte\n", sim_cmd);
	if (sim_busy) {
		fprintf(stderr, "[sim] command 0x%02x sent while the flash is busy: ignored\n", sim_cmd);
		return;
	}
	if (sim_cmd == 0x06) {
		sim_wel = 1;
		return;
	}
	if (sim_cmd != 0x20 && sim_cmd != 0x02) {
		fprintf(stderr, "[sim] flash command 0x%02x is not simulated\n", sim_cmd);
		return;
	}
	if (!sim_wel) {
		fprintf(stderr, "[sim] command 0x%02x without write enable (0x06): ignored\n", sim_cmd);
		return;
	}
	sim_wel = 0;

	if (sim_cmd == 0x20) {
		uint32_t base = sim_addr & ~4095u;

		if (sim_nbytes != 4) {
			fprintf(stderr, "[sim] sector erase with %d bytes: ignored\n", sim_nbytes);
			return;
		}
		if (base < SIM_BASE || base >= SIM_BASE + SIM_SIZE) {
			fprintf(stderr, "[sim] BLOCKED erase outside the PFS region: 0x%06x\n", base);
			return;
		}
		memset(sim_flash + (base - SIM_BASE), 0xFF, 4096);
		sim_erases++;
		sim_busy = SIM_BUSY_ERASE;
		return;
	}

	{
		uint32_t page = sim_addr & ~255u;

		if (sim_nbytes < 5) {
			fprintf(stderr, "[sim] page program without data: ignored\n");
			return;
		}
		if (page < SIM_BASE || page >= SIM_BASE + SIM_SIZE) {
			fprintf(stderr, "[sim] BLOCKED program outside the PFS region: 0x%06x\n", sim_addr);
			return;
		}
		if ((sim_addr & 255u) + (uint32_t)sim_ndata > 256)
			fprintf(stderr, "[sim] page program crosses a page boundary at 0x%06x: wraps like the real chip\n", sim_addr);
		for (int i = 0; i < sim_ndata; i++) {
			uint32_t a = page | ((sim_addr + (uint32_t)i) & 255u);
			uint8_t *cell = &sim_flash[a - SIM_BASE];

			if ((*cell & sim_data[i]) != sim_data[i])
				fprintf(stderr, "[sim] programming 0x%06x without erase: bits cannot go back to 1\n", a);
			*cell &= sim_data[i];
		}
		sim_programs++;
		sim_busy = SIM_BUSY_PROGRAM;
	}
}

// Write to one byte of the spimemio configuration register.
static void sim_spi_wr(int off, uint8_t val)
{
	if (off == 3) {
		int memio = (val & 0x80) != 0;

		if (memio && !sim_memio) {
			if (!sim_cs)
				fprintf(stderr, "[sim] back to memory-mapped mode with CS still low\n");
			if (sim_busy)
				sim_fatal("returned to memory-mapped mode while the flash is still busy");
		}
		sim_memio = memio;
		return;
	}
	if (off != 0)
		return;                             // output enables: not modelled

	{
		int cs = (val >> 5) & 1, clk = (val >> 4) & 1, mosi = val & 1;

		if (sim_memio) {                    // pins not driven by this register yet
			sim_cs = cs;
			sim_clk = clk;
			return;
		}
		if (sim_cs && !cs) {                // CS falls: a command starts
			sim_nbits = 0;
			sim_nbytes = 0;
			sim_out_on = 0;
			sim_miso = 1;
		}
		if (!cs && !sim_cs) {
			if (!sim_clk && clk) {          // rising edge: chip samples MOSI
				sim_shift = (uint8_t)((sim_shift << 1) | mosi);
				if (++sim_nbits == 8) {
					sim_nbits = 0;
					if (!sim_out_on)
						sim_rx_byte(sim_shift);
				}
			}
			if (sim_clk && !clk && sim_out_on) {    // falling edge: chip drives MISO
				if (sim_outbits == 0)
					sim_out = sim_next_out();
				sim_miso = (sim_out >> 7) & 1;
				sim_out = (uint8_t)(sim_out << 1);
				sim_outbits = (sim_outbits + 1) & 7;
			}
		}
		if (!sim_cs && cs)                  // CS rises: the command takes effect
			sim_end_command();
		sim_cs = cs;
		sim_clk = clk;
	}
}

static uint8_t sim_spi_rd(void)
{
	return (uint8_t)((sim_miso << 1) | (sim_cs << 5) | (sim_clk << 4));
}

// ---------------- glue: replace the hardware port of picofs.c ----------------

#undef putchar
#define putchar(c)        sim_putchar(c)
#define reg_uart_data     sim_uart_read()
#define reg_leds          (*sim_mem(0x03000000u))
#define PORT_MEM32(addr)      (*sim_mem(addr))
#define PORT_FLASH_BYTE(a)    sim_flash_byte(a)
#define PORT_SPI_WR(off, v)   sim_spi_wr((off), (uint8_t)(v))
#define PORT_SPI_RD()         sim_spi_rd()
#define PORT_RAMFUNC                    /* a PC cannot run code from .data */
#define PORT_IN_RAM(fn)       1
#define PORT_REBOOT()         sim_reboot()
#define PORT_CYCLES()         sim_cycles()
#define PORT_KEY_PRESSED()    sim_key_pressed()

#include "shell.c"

#ifndef SIM_NO_MAIN
int main(int argc, char **argv)
{
	FILE *f;

	if (PFS_BASE != SIM_BASE || PFS_SLOTS * PFS_SECTOR != SIM_SIZE) {
		fprintf(stderr, "[sim] SIM_BASE / SIM_SIZE do not match picofs.c\n");
		return 1;
	}
	if (argc > 1)
		sim_image = strcmp(argv[1], "-") ? argv[1] : NULL;

	memset(sim_flash, 0xFF, SIM_SIZE);          // erased flash
	if (sim_image && (f = fopen(sim_image, "rb"))) {
		size_t n = fread(sim_flash, 1, SIM_SIZE, f);
		fclose(f);
		fprintf(stderr, "[sim] loaded %u bytes from %s\n", (unsigned)n, sim_image);
	} else {
		fprintf(stderr, "[sim] starting with erased flash\n");
	}

	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &sim_tty_saved) == 0) {
		struct termios raw = sim_tty_saved;

		raw.c_lflag &= ~(ICANON | ECHO);        // the shell echoes by itself
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
		sim_tty_raw = 1;
	}
	atexit(sim_cleanup);
	signal(SIGINT, sim_on_signal);
	signal(SIGTERM, sim_on_signal);

	shell_run();
	return 0;
}
#endif
