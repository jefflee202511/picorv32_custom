// ========================================================
// pico_tetris.c : tetris for the PicoSoC shell
//
// Usage: shell.c includes this file and lists the command  tetris.
//
// KEYS
//   left / right   h l   a d   arrow keys
//   rotate         k     w     arrow up
//   down one row   j     s     arrow down
//   drop           space
//   quit           q           after "game over":  r = play again
//
// SCREEN
//   10 x 20 field, each cell two characters wide ("[]"), on a VT100-style
//   terminal: 23 rows, 60 columns. A move redraws the four cells of the
//   piece; a cleared line redraws the rows above it, which is the largest
//   burst of output the game produces.
//
// NOTES
//   - The game code runs from RAM (TETRIS_RAM), like the editor and snake.
//   - If the first bytes of an arrow key are lost, the last byte ('A'..'D')
//     still works: upper-case A B C D mean rotate, down, right, left.
//   - Timing comes from the CPU cycle counter. No mul/div, no libc.
// ========================================================

#ifndef TETRIS_RAM
#define TETRIS_RAM PORT_RAMFUNC
#endif

#ifndef TETRIS_TICK_MS
#define TETRIS_TICK_MS 500          // time per row at level 1
#endif
#define TETRIS_TICK_MIN  100        // fastest
#define TETRIS_TICK_STEP 40         // faster by this much per level

// Polls to wait for the rest of an escape sequence (a few ms).
#ifndef TETRIS_ESC_SPINS
#define TETRIS_ESC_SPINS 1000
#endif

#define TT_W    10
#define TT_H    20
#define TT_FULL 0x03ff              // all ten cells of a row

#define TT_K_LEFT  1
#define TT_K_RIGHT 2
#define TT_K_ROT   3
#define TT_K_DOWN  4
#define TT_K_DROP  5

// Seven pieces (I O T S Z J L) x four rotations. Each is a 4 x 4 grid in
// 16 bits: bit (y * 4 + x) is the cell in column x, row y.
static const uint16_t tt_shapes[28] = {
	0x00f0, 0x4444, 0x0f00, 0x2222,	// I
	0x0066, 0x0066, 0x0066, 0x0066,	// O
	0x0072, 0x0262, 0x0270, 0x0232,	// T
	0x0036, 0x0462, 0x0360, 0x0231,	// S
	0x0063, 0x0264, 0x0630, 0x0132,	// Z
	0x0071, 0x0226, 0x0470, 0x0322,	// J
	0x0074, 0x0622, 0x0170, 0x0223,	// L
};

static uint16_t tt_rows[TT_H];      // the field: bit x of tt_rows[y] = cell filled
static int tt_piece, tt_rot;        // current piece (0..6) and rotation (0..3)
static int tt_px, tt_py;            // field position of its 4 x 4 grid
static int tt_next;                 // the piece after this one
static int tt_score, tt_lines, tt_level;
static int tt_level_count;          // lines since the last level change
static int tt_tick;                 // ms per row
static uint32_t tt_rng;

// ---------------- helpers ----------------

TETRIS_RAM static void tt_esc(const char *s)
{
	putchar(0x1b);
	io_puts(s);
}

TETRIS_RAM static void tt_goto(int r, int c)
{
	tt_esc("[");
	io_dec((uint32_t)r);
	putchar(';');
	io_dec((uint32_t)c);
	putchar('H');
}

TETRIS_RAM static uint32_t tt_random(void)
{
	tt_rng ^= tt_rng << 13;         // xorshift32
	tt_rng ^= tt_rng >> 17;
	tt_rng ^= tt_rng << 5;
	return tt_rng;
}

TETRIS_RAM static int tt_random_piece(void)
{
	int p;

	do {
		p = (int)(tt_random() & 7);
	} while (p == 7);
	return p;
}

TETRIS_RAM static uint16_t tt_shape(int piece, int rot)
{
	return tt_shapes[(piece << 2) | rot];
}

TETRIS_RAM static int tt_wait_byte(void)
{
	for (int32_t n = TETRIS_ESC_SPINS; n > 0; n--) {
		int32_t c = PORT_UART_TRY();
		if (c != -1)
			return c & 0xff;
	}
	return -1;
}

// Non-blocking. Returns a TT_K_ code, 'q', 'r', or 0.
TETRIS_RAM static int tt_key(void)
{
	int32_t c = PORT_UART_TRY();

	if (c == -1)
		return 0;
	c &= 0xff;
	if (c == 0x1b) {                // arrow key: ESC [ A..D
		c = tt_wait_byte();
		if (c == '[')
			c = tt_wait_byte();
	}
	if (c == 'D' || c == 'h' || c == 'a') return TT_K_LEFT;
	if (c == 'C' || c == 'l' || c == 'd') return TT_K_RIGHT;
	if (c == 'A' || c == 'k' || c == 'w') return TT_K_ROT;
	if (c == 'B' || c == 'j' || c == 's') return TT_K_DOWN;
	if (c == ' ') return TT_K_DROP;
	if (c == 'q' || c == 'Q') return 'q';
	if (c == 'r' || c == 'R') return 'r';
	return 0;
}

// ---------------- drawing ----------------

// Field cell (x, y) is at screen row y + 1, columns 2x + 2 and 2x + 3.
TETRIS_RAM static void tt_cell(int x, int y, int on)
{
	tt_goto(y + 1, (x << 1) + 2);
	io_puts(on ? "[]" : "  ");
}

TETRIS_RAM static void tt_draw_piece(int on)
{
	uint16_t s = tt_shape(tt_piece, tt_rot);

	for (int i = 0; i < 16; i++)
		if ((s >> i) & 1)
			tt_cell(tt_px + (i & 3), tt_py + (i >> 2), on);
}

TETRIS_RAM static void tt_draw_row(int y)
{
	tt_goto(y + 1, 2);
	for (int x = 0; x < TT_W; x++)
		io_puts((tt_rows[y] >> x) & 1 ? "[]" : "  ");
}

TETRIS_RAM static void tt_draw_stats(void)
{
	tt_goto(2, 26);
	io_puts("score ");
	io_dec((uint32_t)tt_score);
	tt_goto(3, 26);
	io_puts("lines ");
	io_dec((uint32_t)tt_lines);
	tt_goto(4, 26);
	io_puts("level ");
	io_dec((uint32_t)tt_level);
}

TETRIS_RAM static void tt_draw_next(void)
{
	uint16_t s = tt_shape(tt_next, 0);

	for (int y = 0; y < 4; y++) {
		tt_goto(7 + y, 26);
		for (int x = 0; x < 4; x++)
			io_puts((s >> ((y << 2) + x)) & 1 ? "[]" : "  ");
	}
}

// ---------------- game rules ----------------

// 1 if the piece cannot be at (px, py) with rotation 'rot'.
TETRIS_RAM static int tt_collide(int px, int py, int rot)
{
	uint16_t s = tt_shape(tt_piece, rot);

	for (int i = 0; i < 16; i++) {
		if ((s >> i) & 1) {
			int x = px + (i & 3), y = py + (i >> 2);

			if (x < 0 || x >= TT_W || y < 0 || y >= TT_H)
				return 1;
			if ((tt_rows[y] >> x) & 1)
				return 1;
		}
	}
	return 0;
}

// Move / rotate the piece if the new place is free. Returns 1 if it moved.
TETRIS_RAM static int tt_move(int px, int py, int rot)
{
	if (tt_collide(px, py, rot))
		return 0;
	tt_draw_piece(0);
	tt_px = px;
	tt_py = py;
	tt_rot = rot;
	tt_draw_piece(1);
	return 1;
}

// Rotate clockwise; if blocked, try one column to the left, then right.
TETRIS_RAM static void tt_rotate(void)
{
	int r = (tt_rot + 1) & 3;

	if (tt_move(tt_px, tt_py, r))
		return;
	if (tt_move(tt_px - 1, tt_py, r))
		return;
	tt_move(tt_px + 1, tt_py, r);
}

// Make the next piece the current one. Returns 0 if there is no room.
TETRIS_RAM static int tt_spawn(void)
{
	tt_piece = tt_next;
	tt_next = tt_random_piece();
	tt_rot = 0;
	tt_px = 3;
	tt_py = 0;
	tt_draw_next();
	if (tt_collide(tt_px, tt_py, tt_rot)) {
		tt_draw_piece(1);
		return 0;
	}
	tt_draw_piece(1);
	return 1;
}

// Fix the piece in the field, remove full rows, bring in the next piece.
// Returns 0 when the game is over.
TETRIS_RAM static int tt_lock(void)
{
	uint16_t s = tt_shape(tt_piece, tt_rot);
	int cleared = 0, lowest = -1;

	for (int i = 0; i < 16; i++)
		if ((s >> i) & 1)
			tt_rows[tt_py + (i >> 2)] |= (uint16_t)(1 << (tt_px + (i & 3)));

	for (int y = TT_H - 1; y >= 0; ) {
		if (tt_rows[y] == TT_FULL) {
			for (int k = y; k > 0; k--)
				tt_rows[k] = tt_rows[k - 1];
			tt_rows[0] = 0;
			cleared++;
			if (lowest < 0)
				lowest = y;
		} else {
			y--;
		}
	}

	if (cleared) {
		for (int y = 0; y <= lowest; y++)
			tt_draw_row(y);
		if (cleared == 1) tt_score += 100;
		if (cleared == 2) tt_score += 300;
		if (cleared == 3) tt_score += 500;
		if (cleared == 4) tt_score += 800;
		tt_lines += cleared;
		tt_level_count += cleared;
		while (tt_level_count >= 10) {
			tt_level_count -= 10;
			tt_level++;
			if (tt_tick > TETRIS_TICK_MIN)
				tt_tick -= TETRIS_TICK_STEP;
		}
		tt_draw_stats();
	}
	return tt_spawn();
}

TETRIS_RAM static void tt_help(int row, const char *text)
{
	tt_goto(row, 26);
	io_puts(text);
}

TETRIS_RAM static void tt_new_game(void)
{
	tt_esc("[2J");
	tt_esc("[?25l");                // hide the cursor

	for (int y = 0; y < TT_H; y++) {
		tt_rows[y] = 0;
		tt_goto(y + 1, 1);
		putchar('#');
		tt_goto(y + 1, (TT_W << 1) + 2);
		putchar('#');
	}
	tt_goto(TT_H + 1, 1);
	for (int x = 0; x < (TT_W << 1) + 2; x++)
		putchar('#');

	tt_help(6, "next");
	tt_help(12, "left/right  h l   a d   arrows");
	tt_help(13, "rotate      k     w     up");
	tt_help(14, "down        j     s     down");
	tt_help(15, "drop        space");
	tt_help(16, "quit        q");

	tt_score = 0;
	tt_lines = 0;
	tt_level = 1;
	tt_level_count = 0;
	tt_tick = TETRIS_TICK_MS;
	tt_draw_stats();

	tt_next = tt_random_piece();
	tt_spawn();
}

// Play until the game ends. Returns 'q' if the player quit.
TETRIS_RAM static int tt_play(void)
{
	uint32_t target = PORT_CYCLES();

	for (;;) {
		for (int i = 0; i < tt_tick; i++)
			target += PORT_CYCLES_PER_MS;

		while ((int32_t)(PORT_CYCLES() - target) < 0) {
			int k = tt_key();

			if (k == 'q')
				return 'q';
			if (k == TT_K_LEFT)
				tt_move(tt_px - 1, tt_py, tt_rot);
			if (k == TT_K_RIGHT)
				tt_move(tt_px + 1, tt_py, tt_rot);
			if (k == TT_K_DOWN)
				tt_move(tt_px, tt_py + 1, tt_rot);
			if (k == TT_K_ROT)
				tt_rotate();
			if (k == TT_K_DROP) {
				while (tt_move(tt_px, tt_py + 1, tt_rot))
					;
				if (!tt_lock())
					return 0;
				target = PORT_CYCLES();     // the new piece gets a full tick
				for (int i = 0; i < tt_tick; i++)
					target += PORT_CYCLES_PER_MS;
			}
		}

		// gravity: one row down, or the piece lands
		if (!tt_move(tt_px, tt_py + 1, tt_rot))
			if (!tt_lock())
				return 0;
	}
}

TETRIS_RAM static int shcmd_tetris(int argc, char **argv)
{
	int k;

#ifdef TETRIS_SEED
	tt_rng = TETRIS_SEED;
#else
	tt_rng = PORT_CYCLES() | 1;             // different every time; never 0
#endif

	for (;;) {
		tt_new_game();
		k = tt_play();
		if (k != 'q') {
			tt_goto(TT_H + 2, 1);
			io_puts("game over - r = play again, q = quit");
			do {
				k = tt_key();
			} while (k != 'q' && k != 'r');
		}
		if (k == 'q')
			break;
	}

	tt_esc("[?25h");                        // show the cursor again
	tt_goto(TT_H + 3, 1);
	putchar('\n');
	return 0;
}

const struct shell_cmd shell_tetris_cmds[] = {
	{ "tetris", shcmd_tetris, "tetris                   play tetris (pico_tetris.c)" },
	{ 0, 0, 0 }
};
