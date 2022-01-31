#include "wayland.h"
#include <stdbool.h>
#include "log.h"
#include "fdio_full.h"
#include <xkbcommon/xkbcommon.h>



/* Code to track keyboard state for modifier masks
 * because the synergy protocol is less than ideal at sending us modifiers
*/


static bool local_mod_init(struct wlContext *wl_ctx, char *keymap_str) {
	wl_ctx->input.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!wl_ctx->input.xkb_ctx) {
		return false;
	}
	wl_ctx->input.xkb_map = xkb_keymap_new_from_string(wl_ctx->input.xkb_ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!wl_ctx->input.xkb_map) {
		xkb_context_unref(wl_ctx->input.xkb_ctx);
		return false;
	}
	wl_ctx->input.xkb_state = xkb_state_new(wl_ctx->input.xkb_map);
	if (!wl_ctx->input.xkb_state) {
		xkb_map_unref(wl_ctx->input.xkb_map);
		xkb_context_unref(wl_ctx->input.xkb_ctx);
		return false;
	}
	/* initialize keystats */
	if (wl_ctx->input.key_press_state) {
		free(wl_ctx->input.key_press_state);
	}
	wl_ctx->input.key_count = xkb_keymap_max_keycode(wl_ctx->input.xkb_map) + 1;
	return true;
}

/* and code to handle raw mapping of keys */

static void load_raw_keymap(struct wlContext *ctx)
{
	char **key, **val, *endstr;
	int i, count, offset, lkey, rkey;
	key = NULL;
	val = NULL;
	if (ctx->input.raw_keymap) {
		free(ctx->input.raw_keymap);
	}
	/* start with the xkb maximum */
	ctx->input.key_count = xkb_keymap_max_keycode(ctx->input.xkb_map);
	logDbg("max key: %zu", ctx->input.key_count);
	if ((count = configReadFullSection("raw-keymap", &key, &val)) != -1) {
		/* slightly inefficient approach, but it will actually work
		 * First pass -- just find the *real* maximum raw keycode */
		for (i = 0; i < count; ++i) {
			errno = 0;
			lkey = strtol(key[i], &endstr, 0);
			if (errno || endstr == key[i])
				continue;
			if (lkey >= ctx->input.key_count) {
				ctx->input.key_count = lkey + 1;
				logDbg("max key update: %zu", ctx->input.key_count);

			}
		}
	}
	/* initialize everything */
	ctx->input.raw_keymap = xcalloc(ctx->input.key_count, sizeof(*ctx->input.raw_keymap));
	offset = configTryLong("raw-keymap/offset", 0);
	offset += configTryLong("xkb_key_offset", 0);
	logDbg("Initial raw key offset: %d", offset);
	for (i = 0; i < ctx->input.key_count; ++i) {
		ctx->input.raw_keymap[i] = i + offset;
	}
	/* initialize key state tracking now that the size is known */
	ctx->input.key_press_state = xcalloc(ctx->input.key_count, sizeof(*ctx->input.key_press_state));
	/* and second pass -- store any actually mappings, apply offset */
	for (i = 0; i < count; ++i) {
		errno = 0;
		lkey = strtol(key[i], &endstr, 0);
		if (errno || endstr == key[i])
			continue;
		errno = 0;
		rkey = strtol(val[i], &endstr, 0);
		if (errno || endstr == val[i])
			continue;
		ctx->input.raw_keymap[lkey] = rkey + offset;
		logDbg("set raw key map: %d = %d", lkey, ctx->input.raw_keymap[lkey]);
	}

	strfreev(key);
	strfreev(val);
}

int wlKeySetConfigLayout(struct wlContext *ctx)
{
	int ret = 0;
	char *keymap_str = configTryStringFull("xkb_keymap", "xkb_keymap { \
		xkb_keycodes  { include \"xfree86+aliases(qwerty)\"     }; \
		xkb_types     { include \"complete\"    }; \
		xkb_compat    { include \"complete\"    }; \
		xkb_symbols   { include \"pc+us+inet(evdev)\"   }; \
		xkb_geometry  { include \"pc(pc105)\"   }; \
};");
	local_mod_init(ctx, keymap_str);
	ret = !ctx->input.key_map(&ctx->input, keymap_str);
	load_raw_keymap(ctx);
	free(keymap_str);
	return ret;
}

void wlKey(struct wlContext *ctx, int key, int state)
{
	if (!ctx->input.key_press_state[key] && !state) {
		return;
	}
	if (key >= ctx->input.key_count) {
		logWarn("Key %d outside configured keymap, dropping", key);
		return;
	}
	ctx->input.key_press_state[key] += state ? 1 : -1;
	key = ctx->input.raw_keymap[key];
	logDbg("Keycode: %d, state %d", key, state);
	if (key > xkb_keymap_max_keycode(ctx->input.xkb_map)) {
		logDbg("keycode greater than xkb maximum, mod not tracked");
	} else {
		xkb_state_update_key(ctx->input.xkb_state, key, state);
		xkb_mod_mask_t depressed = xkb_state_serialize_mods(ctx->input.xkb_state, XKB_STATE_MODS_DEPRESSED);
		xkb_mod_mask_t latched = xkb_state_serialize_mods(ctx->input.xkb_state, XKB_STATE_MODS_LATCHED);
		xkb_mod_mask_t locked = xkb_state_serialize_mods(ctx->input.xkb_state, XKB_STATE_MODS_LOCKED);
		xkb_layout_index_t group = xkb_state_serialize_layout(ctx->input.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
		logDbg("Modifiers: depressed: %x latched: %x locked: %x group: %x", depressed, latched, locked, group);
	}
	ctx->input.key(&ctx->input, key, state);
}

void wlKeyReleaseAll(struct wlContext *ctx)
{
	size_t i;
	for (i = 0; i < ctx->input.key_count; ++i) {
		while (ctx->input.key_press_state[i]) {
			logDbg("Release all: key %zd, pressed %d times", i, ctx->input.key_press_state[i]);
			wlKey(ctx, i, 0);
		}
	}
}


void wlMouseRelativeMotion(struct wlContext *ctx, int dx, int dy)
{
	ctx->input.mouse_rel_motion(&ctx->input, dx, dy);
}
void wlMouseMotion(struct wlContext *ctx, int x, int y)
{
	ctx->input.mouse_motion(&ctx->input, x, y);
}
void wlMouseButtonDown(struct wlContext *ctx, int button)
{
	ctx->input.mouse_button(&ctx->input, button, 1);
}
void wlMouseButtonUp(struct wlContext *ctx, int button)
{
	ctx->input.mouse_button(&ctx->input, button, 0);
}
void wlMouseWheel(struct wlContext *ctx, signed short dx, signed short dy)
{
	ctx->input.mouse_wheel(&ctx->input, dx, dy);
}
