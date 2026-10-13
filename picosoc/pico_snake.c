// ========================================================
// pico_snake.c : snake for the PicoSoC shell
//
// Usage: shell.c includes this file and lists the command  snake.
//
// A UART exercise as much as a game: every tick sends a few bytes
// (cursor positioning + one or two characters), and steering needs the
// receive side to keep up while the game is drawing.
//
// KEYS
//   arrows, or  h j k l,  or  w a s d      steer
//   q  quit        after "game over":  r  play again,  q  quit
//
// SCREEN
//   32 x 16 playing field inside a border, on a VT100-style terminal.
//   '@' head, 'o' body, '*' food. Only the cells that change are drawn.
//
// NOTES
//   - The game code runs from RAM (SNAKE_RAM) so that polling the UART is
//     fast enough to catch the three bytes of an arrow key.
//   - If the first bytes of an arrow key are lost anyway, the last byte
//     ('A'..'D') still steers: upper-case A B C D mean up, down, right, left.
//   - Timing comes from the CPU cycle counter. No mul/div, no libc.
// ========================================================

#ifndef SNAKE_RAM
#define SNAKE_RAM PORT_RAMFUNC
#endif

#ifndef SNAKE_TICK_MS
#define SNAKE_TICK_MS 200           // time per step at the start
#endif
#define SNAKE_TICK_MIN 80           // fastest
#define SNAKE_TICK_STEP 5           // faster by this much per food

// Polls to wait for the rest of an escape sequence (a few ms).
#ifndef SNAKE_ESC_SPINS
#define SNAKE_ESC_SPINS 1000
#endif

#define SN_W     32                 // field width  (power of two: cell = y * 32 + x)
#define SN_H     16                 // field height
#define SN_CELLS (SN_W * SN_H)      // 512

#define SN_EMPTY 0
#define SN_BODY  1
#define SN_FOOD  2

#define SN_UP    1
#define SN_RIGHT 2
#define SN_DOWN  3
#define SN_LEFT  4

static uint8_t  sn_grid[SN_CELLS];  // what is in each cell
static uint16_t sn_body[SN_CELLS];  // ring buffer of the snake's cells, tail first
static int sn_head, sn_tail;        // ring positions of head and tail
static int sn_len;
static int sn_dir, sn_next;         // current direction, direction for the next step
static int sn_score;
static int sn_tick;                 // ms per step
static uint32_t sn_rng;

// ---------------- helpers ----------------

SNAKE_RAM static void sn_esc(const char *s)
{
	putchar(0x1b);
	io_puts(s);
}

SNAKE_RAM static void sn_goto(int r, int c)
{
	sn_esc("[");
	io_dec((uint32_t)r);
	putchar(';');
	io_dec((uint32_t)c);
	putchar('H');
}

// Draw one field cell. The field starts at screen row 2, column 2.
SNAKE_RAM static void sn_cell(int cell, char ch)
{
	sn_goto((cell >> 5) + 2, (cell & 31) + 2);
	putchar(ch);
}

SNAKE_RAM static uint32_t sn_random(void)
{
	sn_rng ^= sn_rng << 13;         // xorshift32
	sn_rng ^= sn_rng >> 17;
	sn_rng ^= sn_rng << 5;
	return sn_rng;
}

SNAKE_RAM static int sn_wait_byte(void)
{
	for (int32_t n = SNAKE_ESC_SPINS; n > 0; n--) {
		int32_t c = PORT_UART_TRY();
		if (c != -1)
			return c & 0xff;
	}
	return -1;
}

// Non-blocking. Returns a direction (SN_UP..SN_LEFT), 'q', 'r', or 0.
SNAKE_RAM static int sn_key(void)
{
	int32_t c = PORT_UART_TRY();

	if (c == -1)
		return 0;
	c &= 0xff;
	if (c == 0x1b) {                // arrow key: ESC [ A..D
		c = sn_wait_byte();
		if (c == '[')
			c = sn_wait_byte();
	}
	if (c == 'A' || c == 'k' || c == 'w') return SN_UP;
	if (c == 'B' || c == 'j' || c == 's') return SN_DOWN;
	if (c == 'C' || c == 'l' || c == 'd') return SN_RIGHT;
	if (c == 'D' || c == 'h' || c == 'a') return SN_LEFT;
	if (c == 'q' || c == 'Q') return 'q';
	if (c == 'r' || c == 'R') return 'r';
	return 0;
}

SNAKE_RAM static void sn_show_score(void)
{
	sn_goto(SN_H + 3, 1);
	io_puts("score: ");
	io_dec((uint32_t)sn_score);
	io_puts("   arrows / hjkl / wasd = steer   q = quit");
	sn_esc("[K");
}

// Put food on a random empty cell. Returns 0 if the field is full.
SNAKE_RAM static int sn_place_food(void)
{
	int cell;

	if (sn_len >= SN_CELLS)
		return 0;
	do {
		cell = (int)(sn_random() & (SN_CELLS - 1));
	} while (sn_grid[cell] != SN_EMPTY);
	sn_grid[cell] = SN_FOOD;
	sn_cell(cell, '*');
	return 1;
}

SNAKE_RAM static void sn_new_game(void)
{
	sn_esc("[2J");
	sn_esc("[?25l");                // hide the cursor

	for (int i = 0; i < SN_CELLS; i++)
		sn_grid[i] = SN_EMPTY;

	sn_goto(1, 1);
	for (int x = 0; x < SN_W + 2; x++)
		putchar('#');
	for (int y = 0; y < SN_H; y++) {
		sn_goto(y + 2, 1);
		putchar('#');
		sn_goto(y + 2, SN_W + 2);
		putchar('#');
	}
	sn_goto(SN_H + 2, 1);
	for (int x = 0; x < SN_W + 2; x++)
		putchar('#');

	// three cells in the middle, heading right
	sn_tail = 0;
	sn_head = 2;
	sn_len = 3;
	for (int i = 0; i < 3; i++) {
		int cell = ((SN_H / 2) << 5) + (SN_W / 2 - 2) + i;

		sn_body[i] = (uint16_t)cell;
		sn_grid[cell] = SN_BODY;
		sn_cell(cell, i == 2 ? '@' : 'o');
	}
	sn_dir = sn_next = SN_RIGHT;
	sn_score = 0;
	sn_tick = SNAKE_TICK_MS;
	sn_show_score();
	sn_place_food();
}

// Advance one cell. Returns 0 when the game is over.
SNAKE_RAM static int sn_step(void)
{
	int cell = sn_body[sn_head];
	int x = cell & 31, y = cell >> 5;
	int eat;

	sn_dir = sn_next;
	if (sn_dir == SN_UP)    y--;
	if (sn_dir == SN_DOWN)  y++;
	if (sn_dir == SN_LEFT)  x--;
	if (sn_dir == SN_RIGHT) x++;
	if (x < 0 || x >= SN_W || y < 0 || y >= SN_H)
		return 0;                           // hit the wall

	cell = (y << 5) + x;
	eat = sn_grid[cell] == SN_FOOD;

	if (!eat) {                             // the tail moves on
		int t = sn_body[sn_tail];

		sn_grid[t] = SN_EMPTY;
		sn_cell(t, ' ');
		sn_tail = (sn_tail + 1) & (SN_CELLS - 1);
	}
	if (sn_grid[cell] == SN_BODY)
		return 0;                           // ran into itself

	sn_cell(sn_body[sn_head], 'o');
	sn_head = (sn_head + 1) & (SN_CELLS - 1);
	sn_body[sn_head] = (uint16_t)cell;
	sn_grid[cell] = SN_BODY;
	sn_cell(cell, '@');

	if (eat) {
		sn_len++;
		sn_score++;
		if (sn_tick > SNAKE_TICK_MIN)
			sn_tick -= SNAKE_TICK_STEP;
		sn_show_score();
		if (!sn_place_food())
			return 0;                       // field full: nothing left to eat
	}
	return 1;
}

// Play until the game ends. Returns 'q' if the player quit.
SNAKE_RAM static int sn_play(void)
{
	uint32_t target = PORT_CYCLES();

	for (;;) {
		for (int i = 0; i < sn_tick; i++)
			target += PORT_CYCLES_PER_MS;

		while ((int32_t)(PORT_CYCLES() - target) < 0) {
			int k = sn_key();

			if (k == 'q')
				return 'q';
			// a direction is accepted unless it reverses the snake
			if ((k == SN_UP && sn_dir != SN_DOWN) || (k == SN_DOWN && sn_dir != SN_UP) ||
					(k == SN_LEFT && sn_dir != SN_RIGHT) || (k == SN_RIGHT && sn_dir != SN_LEFT))
				sn_next = k;
		}
		if (!sn_step())
			return 0;
	}
}

SNAKE_RAM static int shcmd_snake(int argc, char **argv)
{
	int k;

#ifdef SNAKE_SEED
	sn_rng = SNAKE_SEED;
#else
	sn_rng = PORT_CYCLES() | 1;             // different every time; never 0
#endif

	for (;;) {
		sn_new_game();
		k = sn_play();
		if (k != 'q') {
			sn_goto(SN_H + 4, 1);
			io_puts("game over - r = play again, q = quit");
			do {
				k = sn_key();
			} while (k != 'q' && k != 'r');
		}
		if (k == 'q')
			break;
	}

	sn_esc("[?25h");                        // show the cursor again
	sn_goto(SN_H + 5, 1);
	putchar('\n');
	return 0;
}

const struct shell_cmd shell_snake_cmds[] = {
	{ "snake", shcmd_snake, "snake                    play snake (pico_snake.c)" },
	{ 0, 0, 0 }
};
