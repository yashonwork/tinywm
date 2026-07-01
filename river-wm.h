/* See LICENSE file for copyright and license details. */
#include <stdint.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define LENGTH(x)          (sizeof(x) / sizeof(x[0]))
#define TAGMASK            ((1 << 9) - 1)

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct Monitor Monitor;
typedef struct Window Window;
typedef struct Seat Seat;
typedef struct KeyBinding KeyBinding;
typedef struct PointerBinding PointerBinding;

typedef void (*LayoutFn)(Monitor *);

typedef struct {
	const char *symbol;
	LayoutFn arrange;
} Layout;

struct Window {
	struct river_window_v1 *obj;
	struct river_node_v1 *node;

	uint32_t tags;
	int isfloating;
	int isfullscreen;
	int isurgent;

	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int bw;

	int isnew;
	int closed;

	Monitor *mon;
	struct wl_list mlink;
	struct wl_list slink;
	char app_id[256];
	char title[256];

	Seat *move_req;
	Seat *resize_req;
	uint32_t resize_edges;
};

struct Monitor {
	struct river_output_v1 *obj;
	int x, y, w, h;

	uint32_t tagset[2];
	int seltags;

	Window *focus;
	struct wl_list windows;
	struct wl_list stack;

	const Layout *layout;
	int nmaster;
	float mfact;
	int gappx;

	struct wl_list link;
};

struct KeyBinding {
	struct river_xkb_binding_v1 *obj;
	Seat *seat;
	void (*func)(const Arg *);
	Arg arg;
	struct wl_list link;
};

struct PointerBinding {
	struct river_pointer_binding_v1 *obj;
	Seat *seat;
	int is_move; /* 1 = move, 0 = resize */
	struct wl_list link;
};

enum SeatOp {
	OP_NONE,
	OP_MOVE,
	OP_RESIZE,
};

struct Seat {
	struct river_seat_v1 *obj;
	int isnew;
	int removed;

	Window *focus;
	Window *hovered;
	Window *interacted;

	struct wl_list xkb_bindings;
	struct wl_list pointer_bindings;
	KeyBinding *pending;

	enum SeatOp op;
	Window *op_window;
	int32_t op_sx, op_sy;
	int32_t op_dx, op_dy;
	int32_t op_sw, op_sh;
	uint32_t op_edges;
	int op_release;

	struct wl_list link;
};

typedef struct {
	unsigned int mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *app_id;
	const char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
} Rule;

/* global state */
extern struct wl_display *display;
extern struct river_window_manager_v1 *wmgr;
extern struct river_xkb_bindings_v1 *xkb;
extern struct wl_list monitors;
extern struct wl_list globwindows;
extern struct wl_list seats;
extern Monitor *selmon;
extern Seat *selseat;

/* config variables (defined in config.h, included from river-wm.c) */
extern const char *fonts[];
extern const char normbordercolor[];
extern const char selbordercolor[];
extern const unsigned int borderpx;
extern const unsigned int gappx;
extern const float mfact;
extern const int nmaster;
extern const int showbar;
extern const int sloppy;
extern const char *tags[];
extern const unsigned int tagcount;
extern const Rule rules[];
extern const Layout layouts[];
extern const Key keys[];
extern const char *termcmd[];
extern const char *launcmd[];

/* layout functions */
void tile(Monitor *);
void monocle(Monitor *);
void float_(Monitor *);

/* wm functions */
void view(const Arg *);
void tag(const Arg *);
void toggletag(const Arg *);
void viewprevtag(const Arg *);
void focusmaster(const Arg *);
void focusstack(const Arg *);
void swapstack(const Arg *);
void setlayout(const Arg *);
void setmfact(const Arg *);
void togglefloating(const Arg *);
void zoom(const Arg *);
void spawn(const Arg *);
void killclient(const Arg *);
void quitsession(const Arg *);
void arrange(Monitor *);
void focus(Window *, Seat *);
Window *wintofocus(Monitor *, Window *);
void applyrules(Window *);
void cleanup(Window *);
