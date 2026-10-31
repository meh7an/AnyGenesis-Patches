/* Sonic 1 REV00 at 400 pixels: the code side of pack.toml. Each function runs before the
   instruction at its pack address; the addresses and RAM locations come from our own
   reading of the game. */

/* Pixels the view gains on each side; pack.toml's `widescreen` flag and its changed
   immediates are written for the same value. */
#define MARGIN 40u
/* How far left of the camera pack.toml's strips start: the margin and the game's spare
   16 pixels, rounded up to a whole 16-pixel block. */
#define STRIP_REACH 64
#define PLANE_WIDTH 512
/* How close Sonic may come to the left limit. */
#define SONIC_EDGE_ROOM 16
/* Sprite X values (VDP coordinates): the first that wraps round to the left, the last that
   lands wholly left of the picture, and the first left of 0 that lands wholly right of it. */
#define WRAP_RIGHT 512
#define WRAP_TO_LEFT_MARGIN 608
#define WRAP_TO_RIGHT_MARGIN (-64)

#define GAME_MODE 0xFFF600u
#define MODE_SEGA 0x00u
#define MODE_TITLE 0x04u
#define MODE_DEMO 0x08u
#define MODE_LEVEL 0x0Cu
#define MODE_SPECIAL 0x10u
/* Set in the mode byte while a level or a demo loads, its title card included. */
#define MODE_LOADING 0x80u
#define CAMERA_X 0xFFF700u
/* The act's limits as set when it starts, then the current ones, which boss arenas and
   level events move. */
#define START_LEFT_LIMIT 0xFFF720u
#define START_RIGHT_LIMIT 0xFFF722u
#define LEFT_LIMIT 0xFFF728u
#define RIGHT_LIMIT 0xFFF72Au

/* The title screen's masking object: left of the picture it parks 32-pixel sprites that use
   up each line's sprite pixels, so the emblem hides Sonic's lower half; they start 128
   pixels left of the object's X. Its frame number, and an X (VDP coordinates) that puts
   them at -96 to -64 on screen, past the left margin: a sprite counts against its line's
   limits wherever it is. */
#define MASK_FRAME 2u
#define MASK_X 0xA0u
#define OBJECT_FRAME 0x1Au
#define OBJECT_X 0x08u

/* The sprite builder: where it starts on the object in a0, where it moves on to the next
   one, and where it finishes the table. */
#define BUILD_OBJECT 0x00D67Cu
#define BUILD_NEXT 0x00D726u
#define BUILD_DONE 0x00D736u
/* The display queue holds at most 8 layers of 63 objects. */
#define MAX_DEFERRED 504u

/* The special stage draws its blocks from a grid of 16 by 16 rotated cells, 24 pixels
   apart, around the screen's middle: the camera's cell and the 15 after it each way, the
   first at -180 less the camera's place in its cell from the middle. The grid reaches the
   console's picture but not the margins, so a ring of cells around it is worked out here
   with the game's arithmetic and the ring's blocks that land in the margins are drawn too.
   Its loop: where a block's Y is tested and it is drawn, the next block, and the end. */
#define SS_DRAW 0x01B22Eu
#define SS_NEXT_BLOCK 0x01B268u
#define SS_DONE 0x01B276u
#define SS_GRID_CELLS 16
#define SS_CELL 24
#define SS_GRID_START (-180)
/* The ring's depth in cells: 2 reach 240 pixels from the middle along the grid, past the
   wide view's corners (229) at any angle. */
#define SS_RING 2
/* The stage's angle (its low 2 bits ignored), its layout (rows of 0x80 cells), the sine
   table (cosine 64 entries on) and the last block number drawn. */
#define SS_ANGLE 0xFFF780u
#define SS_LAYOUT 0xFF0000u
#define SS_LAYOUT_ROW 0x80
#define SINE_TABLE 0x0029F2u
#define SS_LAST_BLOCK 0x4Eu
/* Unused work RAM during the special stage, where the ring's rotated positions go in the
   game's matrix layout (an X word and a Y word a cell). */
#define SS_RING_MATRIX 0xFFFF8400u
/* What the game adds to a rotated position to make a sprite position. */
#define SS_SPRITE_X 0x120
/* The sprite X range (VDP coordinates) the game draws blocks in, 16 pixels past the
   console's picture, and the one pack.toml widens it to. */
#define SS_LEFT 112
#define SS_RIGHT 464
#define SS_WIDE_LEFT 72
#define SS_WIDE_RIGHT 504
#define SS_MAX_CELLS 400u

/* The water surface object's X before the camera is added: 0x60 for the left one, 0x120
   for the right one. */
#define SURFACE_BASE_X 0x30u
#define SURFACE_LEFT_SHIFT 64u

/* The objects the sprite builder found wholly in the margins this frame, built after all
   the others; `next_deferred` is past the last one built while those are being built. */
static uint32_t deferred[MAX_DEFERRED];
static uint32_t deferred_count;
static uint32_t next_deferred;
static int building_deferred;
/* The special stage blocks only the margins show this frame, drawn after the game's own:
   each one's rotation matrix entry (a4), block number (d0) and sprite X (d3). */
struct MarginBlock {
    uint32_t cell;
    uint32_t block;
    uint32_t x;
};
static struct MarginBlock margin_blocks[SS_MAX_CELLS];
static uint32_t margin_block_count;
static uint32_t next_margin_block;
static int drawing_margin_blocks;
/* The kept blocks of this frame have been drawn and the grid's end is reached again. */
static int margin_blocks_drawn;
/* Set from where the special stage starts to finish (its fade-out and results run with the
   game mode already back at the level's) or a level starts to load until the act's camera
   is set: the margins show whole meanwhile. */
static int view_whole;

static int32_t ram_word(const AgState *s, uint32_t address)
{
    return (int16_t)(uint16_t)ag_ram_read16(s, address);
}

static uint32_t clamp_margin(int32_t pixels)
{
    if (pixels <= 0) {
        return 0u;
    }
    return (uint32_t)pixels < MARGIN ? (uint32_t)pixels : MARGIN;
}

/* Adds `amount` to the low word of a data register, as a 68000 word add would. */
static void add_word(uint32_t *reg, uint32_t amount)
{
    *reg = (*reg & 0xFFFF0000u) | ((*reg + amount) & 0xFFFFu);
}

/* Sets the low word of a data register, as a 68000 word move would. */
static void set_word(uint32_t *reg, int32_t value)
{
    *reg = (*reg & 0xFFFF0000u) | ((uint32_t)value & 0xFFFFu);
}

/* The camera range of the act's level: its limits as the act starts or as level events have
   since widened them, whichever reach further. */
static void act_edges(const AgState *s, int32_t *left, int32_t *right)
{
    int32_t start_left = ram_word(s, START_LEFT_LIMIT);
    int32_t current_left = ram_word(s, LEFT_LIMIT);
    int32_t start_right = ram_word(s, START_RIGHT_LIMIT);
    int32_t current_right = ram_word(s, RIGHT_LIMIT);
    *left = current_left < start_left ? current_left : start_left;
    *right = current_right > start_right ? current_right : start_right;
}

/* The camera limits with the act's left edge moved in by the margin, so the wide view
   reaches that edge just as the game's view did: Sonic is as far from it as in the game and
   the margin's width more shows on the right. The right limit stays the game's: level
   events start boss fights where the camera reaches it. A lock a level event or a boss
   arena sets right of the moved-in edge stays as it is, and one left of it wins over it. */
static void camera_limits(const AgState *s, int32_t *left, int32_t *right)
{
    int32_t edge_left;
    int32_t edge_right;
    act_edges(s, &edge_left, &edge_right);
    int32_t current_left = ram_word(s, LEFT_LIMIT);
    int32_t inner_left = edge_left + (int32_t)MARGIN;
    *right = ram_word(s, RIGHT_LIMIT);
    *left = current_left > inner_left ? current_left : inner_left;
    if (*left > *right) {
        *left = *right;
    }
}

/* At the vertical interrupt, the camera is where the coming frame shows it. Levels and
   demos show the left margin up to the act's left edge, past which the layout holds no
   level, and the right one whole: past the act's right limit the layout holds what follows
   it, or nothing over the background. The Sega screen, the title screen and the special
   stage show theirs whole, and so does everything from where the special stage starts to
   finish, or a level starts to load, until the act's camera is set. Every other screen is
   laid out for 320 pixels and is pillarboxed. */
uint32_t wide_view(AgState *s, uint32_t pc)
{
    uint32_t mode = ag_ram_read8(s, GAME_MODE);
    if (mode == MODE_SEGA || mode == MODE_TITLE || mode == MODE_SPECIAL || view_whole) {
        ag_set_view(s, MARGIN, MARGIN);
        return pc;
    }
    mode &= ~MODE_LOADING;
    if (mode != MODE_LEVEL && mode != MODE_DEMO) {
        ag_set_view(s, 0u, 0u);
        return pc;
    }
    int32_t camera = ram_word(s, CAMERA_X);
    int32_t left;
    int32_t right;
    act_edges(s, &left, &right);
    ag_set_view(s, clamp_margin(camera - left), MARGIN);
    return pc;
}

/* Where an act starts: the camera X the game worked out from Sonic's start, in d1, kept
   within the moved-in limits. From here the margins follow the act's edges again. */
uint32_t wide_camera_start(AgState *s, uint32_t pc)
{
    view_whole = 0;
    int32_t left;
    int32_t right;
    camera_limits(s, &left, &right);
    int32_t camera = (int16_t)(uint16_t)s->d[1];
    camera = camera < left ? left : camera;
    set_word(&s->d[1], camera > right ? right : camera);
    return pc;
}

/* Each frame's scroll first takes Sonic's X on screen less 144 into d0 and branches left on
   a borrow, as if he could never be left of the camera. Near an act's left edge he now can,
   by the margin, so the borrow is taken on the sign instead: left of the camera is "move
   left", and the clamp below keeps the camera where it is. */
uint32_t wide_camera_side(AgState *s, uint32_t pc)
{
    s->c = (uint8_t)((int16_t)(uint16_t)s->d[0] < 0 ? 1u : 0u);
    return pc;
}

/* Each frame's scroll: the new camera X in d0, after the game's own clamp, before it is
   stored over the old one. Like the game's, the clamp holds the side the camera moves
   towards. */
uint32_t wide_camera_step(AgState *s, uint32_t pc)
{
    int32_t left;
    int32_t right;
    camera_limits(s, &left, &right);
    int32_t from = ram_word(s, CAMERA_X);
    int32_t to = (int16_t)(uint16_t)s->d[0];
    if (to > from && to > right) {
        to = right;
    } else if (to < from && to < left) {
        to = left;
    }
    set_word(&s->d[0], to);
    return pc;
}

/* The level's first draw of each plane, 32 blocks a row from the X in d5 against the camera
   word at a3: from STRIP_REACH pixels left of the camera like the strips, but never left of
   the level's X 0, where the layout lookup wraps round to other chunks. A background row
   scrolled faster than its camera, as water is, keeps showing what this draw left in the
   plane. */
uint32_t wide_first_draw(AgState *s, uint32_t pc)
{
    int32_t camera = ram_word(s, s->a[3]);
    s->d[5] = (uint32_t)(camera < STRIP_REACH ? -camera : -STRIP_REACH);
    return pc;
}

/* The background's strips start STRIP_REACH pixels left of its camera word at a3, which is
   left of X 0 near an act's start: the layout lookup there reads the end of the foreground's
   layout row. A background scrolled slower than its camera shows those plane columns in the
   margin, so such a strip starts a plane's width (512 pixels) on instead, at the same plane
   columns. */
uint32_t wide_background_strip(AgState *s, uint32_t pc)
{
    int32_t start = (int16_t)(uint16_t)s->d[5];
    if (ram_word(s, s->a[3]) + start < 0) {
        set_word(&s->d[5], start + PLANE_WIDTH);
    }
    return pc;
}

/* Sonic's left boundary (the left limit and 16 pixels, in d0): 16 pixels into the wide
   view's left edge instead of the console's, as boss arenas and level events lock the
   limit there, but never past the act's own left edge. pack.toml moves the right boundary
   out by the margin as well. */
uint32_t wide_sonic_left_bound(AgState *s, uint32_t pc)
{
    int32_t edge_left;
    int32_t edge_right;
    act_edges(s, &edge_left, &edge_right);
    int32_t bound = (int16_t)(uint16_t)s->d[0] - (int32_t)MARGIN;
    int32_t level = edge_left + SONIC_EDGE_ROOM;
    set_word(&s->d[0], bound > level ? bound : level);
    return pc;
}

/* The sprite builder's left test: an object stays when its right edge is right of the wide
   view's left edge. The right test is pack.toml's changed immediate. */
uint32_t wide_sprite_left(AgState *s, uint32_t pc)
{
    add_word(&s->d[1], MARGIN);
    return pc;
}

/* Where the title's "TM" and masking objects are told apart: the masking one moves left of
   the wide view, so the left margin can show the title's background while it still masks. */
uint32_t wide_title_mask(AgState *s, uint32_t pc)
{
    if (ag_ram_read8(s, s->a[0] + OBJECT_FRAME) == MASK_FRAME) {
        ag_ram_write16(s, s->a[0] + OBJECT_X, MASK_X);
    }
    return pc;
}

/* A sprite piece's X (VDP coordinates, d0) before the builder keeps its low 9 bits: a piece
   left of 0 or at 512 and past wraps round to the other side. The wider culling keeps
   objects that reach into a margin, and their far pieces would wrap into the other margin,
   or the picture. A piece of an object only the margins show is put at X 1, past the left
   margin, whenever it would wrap; one of an object the game draws, only when it would land
   wholly in a margin (from 512 to 608 to the left one, from -64 to -1 to the right one), so
   wherever the console showed it, it shows as before. Put-off pieces still count against
   the line's limits. */
uint32_t wide_sprite_wrap(AgState *s, uint32_t pc)
{
    int32_t x = (int16_t)(uint16_t)s->d[0];
    int wraps = x < 0 || x >= WRAP_RIGHT;
    int margin_only = (x >= WRAP_RIGHT && x <= WRAP_TO_LEFT_MARGIN)
                      || (x >= WRAP_TO_RIGHT_MARGIN && x < 0);
    if (building_deferred ? wraps : margin_only) {
        set_word(&s->d[0], 1);
    }
    return pc;
}

/* The VDP draws a line's sprites in table order until the line's limits, which games use
   for masking, so the table keeps the game's own sprites first, in the game's order: an
   object the game would leave out, wholly in a margin, is built after all the others.
   Here, an object that passed the wide X tests is put off when the game's tests fail. */
uint32_t wide_sprite_defer(AgState *s, uint32_t pc)
{
    if (building_deferred) {
        return pc;
    }
    int32_t x = (int16_t)(uint16_t)s->d[3];
    int32_t half_width = (int32_t)(s->d[0] & 0xFFu);
    if (x + half_width >= 0 && x - half_width < 320) {
        return pc;
    }
    if (deferred_count < MAX_DEFERRED) {
        deferred[deferred_count++] = s->a[0];
    }
    return BUILD_NEXT;
}

/* Where the builder moves on: while the put-off objects are built, to the next of them, or
   back to the end once they all are. */
uint32_t wide_sprite_next(AgState *s, uint32_t pc)
{
    if (!building_deferred) {
        return pc;
    }
    if (next_deferred < deferred_count) {
        s->a[0] = deferred[next_deferred++];
        return BUILD_OBJECT;
    }
    building_deferred = 0;
    deferred_count = 0;
    return BUILD_DONE;
}

/* Where the builder ends the table: the put-off objects are built first, one at a time. */
uint32_t wide_sprite_done(AgState *s, uint32_t pc)
{
    if (building_deferred || deferred_count == 0u) {
        return pc;
    }
    building_deferred = 1;
    next_deferred = 1;
    s->a[0] = deferred[0];
    return BUILD_OBJECT;
}

/* The two "is the object on screen" checks a few objects act on. Biasing the screen X by
   the margin moves the left test out; the right immediates are 320 + 2 * MARGIN to match. */
uint32_t wide_visible_left(AgState *s, uint32_t pc)
{
    add_word(&s->d[0], MARGIN);
    return pc;
}

/* The water surface: two sprites 256 pixels wide each in pack.toml's own mappings, the left
   one moved by two 32-pixel steps, so together they span the wide view on both flicker
   frames with the surface pattern where it was. */
uint32_t wide_surface_x(AgState *s, uint32_t pc)
{
    uint32_t base = ag_ram_read16(s, s->a[0] + SURFACE_BASE_X);
    if (base < 0xC0u) {
        add_word(&s->d[1], 0u - SURFACE_LEFT_SHIFT);
    }
    return pc;
}

/* A grid block about to be tested against the sprite X range (d3): the game's own blocks
   are drawn as it would, in its order; one only the margins show is kept for after them,
   so the game's blocks come first in the sprite table and its 80-sprite limit. */
uint32_t wide_special_block(AgState *s, uint32_t pc)
{
    int32_t x = (int16_t)(uint16_t)s->d[3];
    if (x >= SS_LEFT && x < SS_RIGHT) {
        return pc;
    }
    if (x >= SS_WIDE_LEFT && x < SS_WIDE_RIGHT && margin_block_count < SS_MAX_CELLS) {
        struct MarginBlock *kept = &margin_blocks[margin_block_count++];
        kept->cell = s->a[4];
        kept->block = s->d[0];
        kept->x = s->d[3];
    }
    return SS_NEXT_BLOCK;
}

/* Starts drawing the kept block at `index`, from its Y test on. */
static uint32_t draw_margin_block(AgState *s, uint32_t index)
{
    const struct MarginBlock *kept = &margin_blocks[index];
    s->a[4] = kept->cell;
    s->d[0] = kept->block;
    s->d[3] = kept->x;
    return SS_DRAW;
}

/* The next block: while the kept blocks are drawn, the next of them, or the end once they
   all are. */
uint32_t wide_special_next(AgState *s, uint32_t pc)
{
    if (!drawing_margin_blocks) {
        return pc;
    }
    if (next_margin_block < margin_block_count) {
        return draw_margin_block(s, next_margin_block++);
    }
    drawing_margin_blocks = 0;
    margin_block_count = 0;
    margin_blocks_drawn = 1;
    return SS_DONE;
}

/* A 68000 word as a signed value. */
static int32_t signed_word(uint32_t value)
{
    return (int16_t)(uint16_t)value;
}

/* Keeps the ring cell `row`, `column` (outside the grid's 0 to 15) for drawing when its
   block lands in a margin: its rotated position, as the game's loop would work it out, goes
   into the ring's matrix entry `slot`. */
static void keep_ring_cell(AgState *s, int32_t row, int32_t column, uint32_t *slot)
{
    int32_t camera_x = (int32_t)ag_ram_read16(s, CAMERA_X);
    int32_t camera_y = (int32_t)ag_ram_read16(s, CAMERA_X + 4u);
    uint32_t cell = SS_LAYOUT + (uint32_t)((camera_y / SS_CELL + row) * SS_LAYOUT_ROW)
                    + (uint32_t)(camera_x / SS_CELL + column);
    uint32_t block = ag_ram_read8(s, cell);
    if (block == 0u || block > SS_LAST_BLOCK || margin_block_count >= SS_MAX_CELLS) {
        return;
    }
    uint32_t angle = ag_ram_read8(s, SS_ANGLE) & 0xFCu;
    int32_t sine = signed_word(ag_rom_read16(s, SINE_TABLE + angle * 2u));
    int32_t cosine = signed_word(ag_rom_read16(s, SINE_TABLE + angle * 2u + 0x80u));
    int32_t x = signed_word((uint32_t)(SS_GRID_START - camera_x % SS_CELL + column * SS_CELL));
    int32_t y = signed_word((uint32_t)(SS_GRID_START - camera_y % SS_CELL + row * SS_CELL));
    int32_t rotated_x = (-sine * y + cosine * x) >> 8;
    int32_t rotated_y = (sine * x + cosine * y) >> 8;
    int32_t sprite_x = signed_word((uint32_t)(rotated_x + SS_SPRITE_X));
    if ((sprite_x >= SS_LEFT && sprite_x < SS_RIGHT)
        || sprite_x < SS_WIDE_LEFT
        || sprite_x >= SS_WIDE_RIGHT) {
        return;
    }
    uint32_t entry = SS_RING_MATRIX + *slot * 4u;
    *slot += 1u;
    ag_ram_write16(s, entry, (uint32_t)rotated_x & 0xFFFFu);
    ag_ram_write16(s, entry + 2u, (uint32_t)rotated_y & 0xFFFFu);
    struct MarginBlock *kept = &margin_blocks[margin_block_count++];
    kept->cell = entry;
    kept->block = block;
    kept->x = (uint32_t)sprite_x & 0xFFFFu;
}

/* The end of the grid: the ring's margin blocks are added to the kept ones, which are drawn
   before the table ends. */
uint32_t wide_special_done(AgState *s, uint32_t pc)
{
    if (margin_blocks_drawn) {
        margin_blocks_drawn = 0;
        return pc;
    }
    uint32_t slot = 0u;
    for (int32_t row = -SS_RING; row < SS_GRID_CELLS + SS_RING; row++) {
        int32_t inside = row >= 0 && row < SS_GRID_CELLS;
        for (int32_t column = -SS_RING; column < SS_GRID_CELLS + SS_RING; column++) {
            if (!inside || column < 0 || column >= SS_GRID_CELLS) {
                keep_ring_cell(s, row, column, &slot);
            }
        }
    }
    if (margin_block_count == 0u) {
        return pc;
    }
    drawing_margin_blocks = 1;
    next_margin_block = 1;
    return draw_margin_block(s, 0);
}

/* Where the special stage starts to finish: from here until the next act's camera is set,
   the margins show whole. */
uint32_t wide_special_finish(AgState *s, uint32_t pc)
{
    (void)s;
    view_whole = 1;
    return pc;
}

/* Where a level starts to load: until its act's camera is set, the margins show whole. */
uint32_t wide_level_load(AgState *s, uint32_t pc)
{
    (void)s;
    view_whole = 1;
    return pc;
}
