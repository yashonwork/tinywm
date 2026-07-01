/* appearance */
const unsigned int borderpx   = 2;
const unsigned int gappx      = 6;
const float        mfact      = 0.55f;
const int          nmaster    = 1;
const int          showbar    = 1;
const int          sloppy     = 0;

const char *fonts[] = { "monospace:size=10" };

const char normbordercolor[] = "#828bb8";
const char selbordercolor[]  = "#ffc777";

/* tagging */
const char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
const unsigned int tagcount = LENGTH(tags);

const Rule rules[] = {
	{ "firefox",    NULL, 1 << 1, 0, -1 },
	{ "zenity",     NULL, 0,     1, -1 },
	{ NULL,         NULL, 0,     0, -1 },
};

/* layout(s) */
const Layout layouts[] = {
	{ "[]=", tile },
	{ "[M]", monocle },
	{ "><>", float_ },
};

/* key definitions */
#define MODKEY RIVER_SEAT_V1_MODIFIERS_MOD4
#define TAGKEYS(K, T, M) \
	{ MODKEY,             K, view,        { .ui = 1 << T } }, \
	{ MODKEY|M,           K, tag,         { .ui = 1 << T } }, \
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_CTRL, K, toggletag, { .ui = 1 << T } }

const Key keys[] = {
	/* terminal */
	{ MODKEY,         XKB_KEY_Return,    spawn,        { .v = termcmd } },
	{ MODKEY,         XKB_KEY_p,         spawn,        { .v = launcmd  } },

	/* navigation */
	{ MODKEY,         XKB_KEY_j,         focusstack,   { .i = +1 } },
	{ MODKEY,         XKB_KEY_k,         focusstack,   { .i = -1 } },
	{ MODKEY,         XKB_KEY_i,         focusmaster,  { .i = 0 } },
	{ MODKEY,         XKB_KEY_Tab,       viewprevtag,  { .i = 0 } },

	/* swap windows */
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_j, swapstack, { .i = +1 } },
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_k, swapstack, { .i = -1 } },

	/* close */
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_q, killclient, { .i = 0 } },

	/* layouts */
	{ MODKEY,         XKB_KEY_space,     setlayout,    { .v = &layouts[0] } },
	{ MODKEY,         XKB_KEY_m,         setlayout,    { .v = &layouts[1] } },
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_f, togglefloating, { .i = 0 } },

	/* master */
	{ MODKEY,         XKB_KEY_h,         setmfact,     { .f = -0.05f } },
	{ MODKEY,         XKB_KEY_l,         setmfact,     { .f = +0.05f } },
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_Return, zoom, { .i = 0 } },

	/* tags */
	TAGKEYS(          XKB_KEY_1,         0,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_2,         1,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_3,         2,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_4,         3,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_5,         4,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_6,         5,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_7,         6,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_8,         7,            RIVER_SEAT_V1_MODIFIERS_SHIFT),
	TAGKEYS(          XKB_KEY_9,         8,            RIVER_SEAT_V1_MODIFIERS_SHIFT),

	/* quit */
	{ MODKEY|RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_Escape, quitsession, { .i = 0 } },
};

const char *termcmd[] = { "foot", NULL };
const char *launcmd[] = { "wmenu-run", NULL };
