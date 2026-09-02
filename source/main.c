/*---------------------------------------------------------------------------------
	Cloud Climb
	A vertical-jumper game for Wii homebrew, built with devkitPPC + libogc + GRRLIB.

	Player PNGs are embedded directly into the .dol at build time (via the
	devkitPPC "data" folder mechanism) instead of being loaded from the SD
	card at runtime. Put these files in data/ before building:
		data/player_still.png  (standing / near the top of a jump)
		data/player_move.png   (rising or falling)
		data/player_death.png  (shown on the Game Over screen)

	The build system (see Makefile, DATA := data) automatically turns each
	file into an object file + header -- e.g. data/player_still.png becomes
	"player_still_png.h", declaring `extern const u8 player_still_png[];`.
	That symbol is what GRRLIB_LoadTexture() takes directly, no file I/O
	needed at runtime, and no SD card required to run the game at all.

	The background has no image asset -- it's a solid sky-blue fill drawn
	directly in code (see DrawScene). Platforms are the same: no image
	asset, generated entirely in code as colored rounded bars.

	Controls:
		D-Pad Left / Right : move player left / right
		HOME               : quit to loader
		A (on game over)   : restart
---------------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <grrlib.h>
#include <wiiuse/wpad.h>

#include "player_still_png.h"
#include "player_move_png.h"
#include "player_death_png.h"

/*---------------------------------------------------------------------------------
	Config
---------------------------------------------------------------------------------*/
#define SCREEN_W        640
#define SCREEN_H        480

#define MAX_PLATFORMS   16
#define PLATFORM_H      16
#define PLATFORM_MIN_W  50
#define PLATFORM_MAX_W  90
#define PLAYER_W        48
#define PLAYER_H        48

#define GRAVITY         0.35f
#define JUMP_VELOCITY   -11.0f
#define MOVE_SPEED      4.5f
#define SCROLL_LINE     (SCREEN_H * 0.4f)
#define STILL_VY_THRESHOLD  2.0f  /* |vy| below this counts as "still" (near jump apex) */

typedef enum { PLAT_NORMAL, PLAT_BREAKABLE } PlatformType;

typedef struct {
	float x, y;
	float w;         /* randomized per platform, drawn procedurally */
	u32   color;      /* randomized per platform, drawn procedurally */
	int   active;
	PlatformType type;
} Platform;

typedef struct {
	float x, y;
	float vx, vy;
	int   facing; /* -1 left, 0 idle, 1 right -- used only to flip the sprite */
} Player;

typedef enum { STATE_PLAYING, STATE_GAMEOVER } GameState;

/*---------------------------------------------------------------------------------
	Globals
---------------------------------------------------------------------------------*/
static GRRLIB_texImg *texPlayerStill = NULL;
static GRRLIB_texImg *texPlayerMove  = NULL;
static GRRLIB_texImg *texPlayerDeath = NULL;

static Platform platforms[MAX_PLATFORMS];
static Player   player;
static GameState state = STATE_PLAYING;
static long      score = 0;
static long      bestScore = 0;

/* A handful of pleasant, distinguishable platform colors (RGBA), picked at
   random per platform so the procedurally-generated platforms don't look
   flat/samey without any art. */
static const u32 kPlatformColors[] = {
	0x8BD450FF, /* green    */
	0x5CB8E8FF, /* sky blue */
	0xE8C15CFF, /* sandy    */
	0xE87A9CFF, /* pink     */
	0xB98BE8FF, /* lavender */
};
#define PLATFORM_COLOR_COUNT (sizeof(kPlatformColors) / sizeof(kPlatformColors[0]))
#define PLATFORM_BREAK_COLOR 0xE85C5CFF /* red -- always used for breakable platforms */

/*---------------------------------------------------------------------------------
	Load one of the PNGs embedded into the .dol at compile time into a
	GRRLIB texture. `data` is the extern array from the generated header
	(e.g. player_still_png). Returns NULL (and logs) if decoding fails.
---------------------------------------------------------------------------------*/
static GRRLIB_texImg *LoadEmbeddedPNG(const u8 *data, const char *label) {
	GRRLIB_texImg *tex = GRRLIB_LoadTexture(data);
	if (tex) {
		GRRLIB_InitTileSet(tex, tex->w, tex->h, 0);
	} else {
		printf("[CloudClimb] failed to decode embedded asset: %s\n", label);
	}
	return tex;
}

/*---------------------------------------------------------------------------------
	Platform helpers -- fully procedural, no image assets involved.
---------------------------------------------------------------------------------*/
static void SpawnPlatform(float x, float y) {
	for (int i = 0; i < MAX_PLATFORMS; i++) {
		if (!platforms[i].active) {
			platforms[i].x = x;
			platforms[i].y = y;
			platforms[i].w = (float)(PLATFORM_MIN_W + (rand() % (PLATFORM_MAX_W - PLATFORM_MIN_W)));
			platforms[i].active = 1;

			/* ~1 in 5 platforms are breakable */
			if ((rand() % 5) == 0) {
				platforms[i].type  = PLAT_BREAKABLE;
				platforms[i].color = PLATFORM_BREAK_COLOR;
			} else {
				platforms[i].type  = PLAT_NORMAL;
				platforms[i].color = kPlatformColors[rand() % PLATFORM_COLOR_COUNT];
			}
			return;
		}
	}
}

static void InitPlatforms(void) {
	memset(platforms, 0, sizeof(platforms));

	/* Guaranteed platform right under the player so the game starts fairly */
	SpawnPlatform(SCREEN_W / 2 - PLATFORM_MIN_W / 2, SCREEN_H - 40);

	float y = SCREEN_H - 90;
	while (y > -20) {
		float x = (float)(rand() % (SCREEN_W - PLATFORM_MAX_W));
		SpawnPlatform(x, y);
		y -= 55 + (rand() % 40); /* uneven vertical gaps */
	}
}

/*---------------------------------------------------------------------------------
	Game setup / reset
---------------------------------------------------------------------------------*/
static void ResetGame(void) {
	player.x = SCREEN_W / 2.0f - PLAYER_W / 2.0f;
	player.y = SCREEN_H - 100.0f;
	player.vx = 0;
	player.vy = JUMP_VELOCITY;
	player.facing = 0;

	InitPlatforms();

	score = 0;
	state = STATE_PLAYING;
}

/*---------------------------------------------------------------------------------
	Update
---------------------------------------------------------------------------------*/
static void UpdatePlaying(void) {
	WPAD_ScanPads();
	u32 held = WPAD_ButtonsHeld(0);

	/* Movement */
	if (held & WPAD_BUTTON_LEFT) {
		player.vx = -MOVE_SPEED;
		player.facing = -1;
	} else if (held & WPAD_BUTTON_RIGHT) {
		player.vx = MOVE_SPEED;
		player.facing = 1;
	} else {
		player.vx *= 0.8f; /* light friction so stopping feels natural */
	}

	player.x += player.vx;

	/* Wrap around screen edges, classic vertical-jumper style */
	if (player.x + PLAYER_W < 0) player.x = SCREEN_W;
	if (player.x > SCREEN_W)     player.x = -PLAYER_W;

	/* Gravity */
	player.vy += GRAVITY;
	player.y += player.vy;

	/* Collide with platforms only while falling (vy > 0) */
	if (player.vy > 0) {
		for (int i = 0; i < MAX_PLATFORMS; i++) {
			if (!platforms[i].active) continue;

			int withinX = (player.x + PLAYER_W * 0.3f < platforms[i].x + platforms[i].w) &&
			              (player.x + PLAYER_W * 0.7f > platforms[i].x);
			int feetY    = player.y + PLAYER_H;
			int landing  = (feetY >= platforms[i].y) &&
			               (feetY <= platforms[i].y + PLATFORM_H + player.vy);

			if (withinX && landing) {
				if (platforms[i].type == PLAT_BREAKABLE) {
					platforms[i].active = 0; /* breaks, no bounce */
				} else {
					player.vy = JUMP_VELOCITY;
				}
			}
		}
	}

	/* Scroll world up when player climbs above the scroll line */
	if (player.y < SCROLL_LINE) {
		float dy = SCROLL_LINE - player.y;
		player.y = SCROLL_LINE;
		score += (long)dy;

		for (int i = 0; i < MAX_PLATFORMS; i++) {
			if (!platforms[i].active) continue;
			platforms[i].y += dy;
			if (platforms[i].y > SCREEN_H) {
				/* recycle off the top with a fresh random platform */
				platforms[i].active = 0;
				SpawnPlatform((float)(rand() % (SCREEN_W - PLATFORM_MAX_W)), -20.0f);
			}
		}
	}

	/* Death: fell below the bottom of the screen */
	if (player.y > SCREEN_H) {
		if (score > bestScore) bestScore = score;
		state = STATE_GAMEOVER;
	}
}

static void UpdateGameOver(void) {
	WPAD_ScanPads();
	u32 pressed = WPAD_ButtonsDown(0);
	if (pressed & WPAD_BUTTON_A) {
		ResetGame();
	}
}

/*---------------------------------------------------------------------------------
	Draw
---------------------------------------------------------------------------------*/
static void DrawPlatform(Platform *p) {
	/* Fully procedural -- no image asset. A rounded look is faked with a
	   filled rectangle plus small corner circles. */
	GRRLIB_Rectangle(p->x, p->y, p->w, PLATFORM_H, p->color, 1);
	GRRLIB_Circle(p->x,          p->y + PLATFORM_H / 2, PLATFORM_H / 2, p->color, 1);
	GRRLIB_Circle(p->x + p->w,   p->y + PLATFORM_H / 2, PLATFORM_H / 2, p->color, 1);
}

static void DrawPlayer(void) {
	GRRLIB_texImg *tex;

	if (state == STATE_GAMEOVER) {
		tex = texPlayerDeath;
	} else if (fabsf(player.vy) < STILL_VY_THRESHOLD) {
		tex = texPlayerStill;
	} else {
		tex = texPlayerMove;
	}

	if (tex) {
		if (player.facing < 0) {
			/* flip horizontally: negative scaleX + shift x by the sprite width */
			GRRLIB_DrawImg(player.x + PLAYER_W, player.y, tex, 0, -1, 1, 0xFFFFFFFF);
		} else {
			GRRLIB_DrawImg(player.x, player.y, tex, 0, 1, 1, 0xFFFFFFFF);
		}
	} else {
		/* fallback if the relevant PNG wasn't found: solid orange box */
		GRRLIB_Rectangle(player.x, player.y, PLAYER_W, PLAYER_H, 0xFF6600FF, 1);
	}
}

static void DrawScene(void) {
	GRRLIB_FillScreen(0x87CEEBFF); /* solid sky-blue, no image asset */

	for (int i = 0; i < MAX_PLATFORMS; i++) {
		if (platforms[i].active) DrawPlatform(&platforms[i]);
	}

	DrawPlayer();

	char buf[64];
	snprintf(buf, sizeof(buf), "Score: %ld", score);
	GRRLIB_Printf(10, 10, GRRLIB_DefaultTexFont, 1, 1, 0x000000FF, buf);
}

static void DrawGameOver(void) {
	DrawScene(); /* draw the frozen final scene (with the death sprite) behind the overlay */

	GRRLIB_Rectangle(SCREEN_W/2 - 140, SCREEN_H/2 - 60, 280, 120, 0x000000AA, 1);

	GRRLIB_Printf(SCREEN_W/2 - 90, SCREEN_H/2 - 30, GRRLIB_DefaultTexFont, 1, 1, 0xFFFFFFFF, "GAME OVER");

	char buf[64];
	snprintf(buf, sizeof(buf), "Score: %ld", score);
	GRRLIB_Printf(SCREEN_W/2 - 90, SCREEN_H/2, GRRLIB_DefaultTexFont, 1, 1, 0xFFFFFFFF, buf);

	snprintf(buf, sizeof(buf), "Best: %ld", bestScore);
	GRRLIB_Printf(SCREEN_W/2 - 90, SCREEN_H/2 + 15, GRRLIB_DefaultTexFont, 1, 1, 0xFFFFFFFF, buf);

	GRRLIB_Printf(SCREEN_W/2 - 90, SCREEN_H/2 + 35, GRRLIB_DefaultTexFont, 1, 1, 0xFFFF00FF, "Press A to restart");
}

/*---------------------------------------------------------------------------------
	main
---------------------------------------------------------------------------------*/
int main(int argc, char **argv) {
	GRRLIB_Init();
	WPAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);

	srand(time(NULL));

	/* Decode the PNGs that got embedded into the .dol at build time.
	   Platforms need no assets at all. */
	texPlayerStill = LoadEmbeddedPNG(player_still_png,  "player_still.png");
	texPlayerMove  = LoadEmbeddedPNG(player_move_png,   "player_move.png");
	texPlayerDeath = LoadEmbeddedPNG(player_death_png,  "player_death.png");

	ResetGame();

	while (1) {
		WPAD_ScanPads();
		u32 pressed = WPAD_ButtonsDown(0);
		if (pressed & WPAD_BUTTON_HOME) break;

		if (state == STATE_PLAYING) {
			UpdatePlaying();
			DrawScene();
		} else {
			UpdateGameOver();
			DrawGameOver();
		}

		GRRLIB_Render();
	}

	/* Cleanup */
	if (texPlayerStill) GRRLIB_FreeTexture(texPlayerStill);
	if (texPlayerMove)  GRRLIB_FreeTexture(texPlayerMove);
	if (texPlayerDeath) GRRLIB_FreeTexture(texPlayerDeath);

	GRRLIB_Exit();
	exit(0);
	return 0;
}
