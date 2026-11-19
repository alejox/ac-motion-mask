/*
AC Motion Mask
Copyright (C) 2026 Alejandro Grajales (alejoxcode)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <math.h>

#define FILTER_ID "ac_motion_mask_filter"

extern void acm_open_support_dialog(void);
#define T_(key) obs_module_text(key)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum mask_type {
	MASK_NONE = 0,
	MASK_RECT = 1,
	MASK_CIRCLE = 2,
	MASK_ELLIPSE = 3,
	MASK_IMAGE = 4,
	MASK_GRADIENT = 5,
};

enum anim_type {
	ANIM_NONE = 0,
	ANIM_FADE,
	ANIM_ZOOM,
	ANIM_MASK,
	ANIM_SLIDE_LEFT,
	ANIM_SLIDE_RIGHT,
	ANIM_SLIDE_UP,
	ANIM_SLIDE_DOWN,
	ANIM_WIPE_RIGHT,
	ANIM_WIPE_LEFT,
	ANIM_WIPE_DOWN,
	ANIM_WIPE_UP,
};

enum easing_type {
	EASE_LINEAR = 0,
	EASE_OUT,
	EASE_IN_OUT,
	EASE_BACK,
	EASE_ELASTIC,
	EASE_BOUNCE,
};

enum trigger_type {
	TRIGGER_ON_SHOW = 0,
	TRIGGER_MANUAL = 1,
};

enum anim_state {
	STATE_SHOWN = 0,
	STATE_IN,
	STATE_OUT,
	STATE_HIDDEN,
};

enum pending_cmd {
	CMD_NONE = 0,
	CMD_IN,
	CMD_OUT,
	CMD_TOGGLE,
};

struct motion_mask {
	obs_source_t *context;
	gs_effect_t *effect;

	/* mask settings */
	int mask_type;
	struct vec2 mask_center;
	struct vec2 mask_size;
	struct vec4 corner_radii; /* tl, tr, br, bl */
	float feather;
	bool invert;
	int image_channel;
	float grad_angle;
	float grad_pos;
	float grad_width;

	char *image_path;
	gs_texture_t *image_tex;

	/* animation settings */
	int trigger;
	int anim_in;
	int anim_out;
	int easing;
	float duration;
	float delay;

	/* animation state (graphics thread) */
	enum anim_state state;
	float elapsed;
	bool was_showing;
	volatile long pending;

	/* hotkeys, registered on the parent source */
	bool hotkeys_registered;
	obs_hotkey_id hotkey_in;
	obs_hotkey_id hotkey_out;
	obs_hotkey_id hotkey_toggle;

	/* values sent to the shader */
	bool draw_nothing;
	struct vec2 move_offset;
	float move_scale;
	float opacity;
	float mask_scale;
	bool wipe_enabled;
	struct vec2 wipe_dir;
	float wipe_progress;

	gs_eparam_t *p_mask_image;
	gs_eparam_t *p_mask_type;
	gs_eparam_t *p_source_size;
	gs_eparam_t *p_mask_center;
	gs_eparam_t *p_mask_size;
	gs_eparam_t *p_corner_radii;
	gs_eparam_t *p_feather;
	gs_eparam_t *p_invert_mask;
	gs_eparam_t *p_image_channel;
	gs_eparam_t *p_grad_dir;
	gs_eparam_t *p_grad_pos;
	gs_eparam_t *p_grad_width;
	gs_eparam_t *p_move_offset;
	gs_eparam_t *p_move_scale;
	gs_eparam_t *p_opacity;
	gs_eparam_t *p_wipe_enabled;
	gs_eparam_t *p_wipe_dir;
	gs_eparam_t *p_wipe_progress;
	gs_eparam_t *p_wipe_soft;
};

/* ------------------------------------------------------------------ */
/* easing                                                              */

static float ease_bounce_out(float t)
{
	const float n1 = 7.5625f;
	const float d1 = 2.75f;

	if (t < 1.0f / d1) {
		return n1 * t * t;
	} else if (t < 2.0f / d1) {
		t -= 1.5f / d1;
		return n1 * t * t + 0.75f;
	} else if (t < 2.5f / d1) {
		t -= 2.25f / d1;
		return n1 * t * t + 0.9375f;
	}
	t -= 2.625f / d1;
	return n1 * t * t + 0.984375f;
}

static float apply_easing(int easing, float t)
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	switch (easing) {
	case EASE_OUT: {
		float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case EASE_IN_OUT:
		if (t < 0.5f)
			return 4.0f * t * t * t;
		else {
			float u = -2.0f * t + 2.0f;
			return 1.0f - u * u * u / 2.0f;
		}
	case EASE_BACK: {
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	case EASE_ELASTIC: {
		const float c4 = (float)(2.0 * M_PI / 3.0);
		return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
	}
	case EASE_BOUNCE:
		return ease_bounce_out(t);
	case EASE_LINEAR:
	default:
		return t;
	}
}

static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ------------------------------------------------------------------ */
/* animation                                                           */

static void start_anim(struct motion_mask *f, enum anim_state state)
{
	f->state = state;
	f->elapsed = 0.0f;
}

static void reset_motion(struct motion_mask *f)
{
	f->draw_nothing = false;
	vec2_zero(&f->move_offset);
	f->move_scale = 1.0f;
	f->opacity = 1.0f;
	f->mask_scale = 1.0f;
	f->wipe_enabled = false;
	vec2_set(&f->wipe_dir, 1.0f, 0.0f);
	f->wipe_progress = 1.0f;
}

/* v: 0 = fully hidden, 1 = fully shown (may overshoot with some easings) */
static void apply_anim(struct motion_mask *f, int anim, float v)
{
	switch (anim) {
	case ANIM_FADE:
		f->opacity = clamp01(v);
		break;
	case ANIM_ZOOM:
		f->move_scale = v < 0.0f ? 0.0f : v;
		f->opacity = clamp01(v * 2.0f);
		break;
	case ANIM_MASK:
		if (f->mask_type == MASK_NONE) {
			f->opacity = clamp01(v);
		} else if (f->mask_type == MASK_GRADIENT) {
			float rad = f->grad_angle * (float)(M_PI / 180.0);
			f->wipe_enabled = true;
			vec2_set(&f->wipe_dir, cosf(rad), sinf(rad));
			f->wipe_progress = clamp01(v);
		} else {
			f->mask_scale = v < 0.0f ? 0.0f : v;
		}
		break;
	case ANIM_SLIDE_LEFT:
		f->move_offset.x = -(1.0f - v);
		break;
	case ANIM_SLIDE_RIGHT:
		f->move_offset.x = 1.0f - v;
		break;
	case ANIM_SLIDE_UP:
		f->move_offset.y = -(1.0f - v);
		break;
	case ANIM_SLIDE_DOWN:
		f->move_offset.y = 1.0f - v;
		break;
	case ANIM_WIPE_RIGHT:
	case ANIM_WIPE_LEFT:
	case ANIM_WIPE_DOWN:
	case ANIM_WIPE_UP:
		f->wipe_enabled = true;
		f->wipe_progress = clamp01(v);
		if (anim == ANIM_WIPE_RIGHT)
			vec2_set(&f->wipe_dir, 1.0f, 0.0f);
		else if (anim == ANIM_WIPE_LEFT)
			vec2_set(&f->wipe_dir, -1.0f, 0.0f);
		else if (anim == ANIM_WIPE_DOWN)
			vec2_set(&f->wipe_dir, 0.0f, 1.0f);
		else
			vec2_set(&f->wipe_dir, 0.0f, -1.0f);
		break;
	case ANIM_NONE:
	default:
		break;
	}
}

static void motion_mask_hotkey_in(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct motion_mask *f = data;
	if (pressed)
		os_atomic_set_long(&f->pending, CMD_IN);
}

static void motion_mask_hotkey_out(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct motion_mask *f = data;
	if (pressed)
		os_atomic_set_long(&f->pending, CMD_OUT);
}

static void motion_mask_hotkey_toggle(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct motion_mask *f = data;
	if (pressed)
		os_atomic_set_long(&f->pending, CMD_TOGGLE);
}

static void register_hotkeys(struct motion_mask *f, obs_source_t *parent)
{
	const char *filter_name = obs_source_get_name(f->context);
	struct dstr name = {0};
	struct dstr desc = {0};

	dstr_printf(&name, "ac_motion_mask.in.%s", filter_name);
	dstr_printf(&desc, "%s: %s", filter_name, T_("Hotkey.In"));
	f->hotkey_in = obs_hotkey_register_source(parent, name.array, desc.array, motion_mask_hotkey_in, f);

	dstr_printf(&name, "ac_motion_mask.out.%s", filter_name);
	dstr_printf(&desc, "%s: %s", filter_name, T_("Hotkey.Out"));
	f->hotkey_out = obs_hotkey_register_source(parent, name.array, desc.array, motion_mask_hotkey_out, f);

	dstr_printf(&name, "ac_motion_mask.toggle.%s", filter_name);
	dstr_printf(&desc, "%s: %s", filter_name, T_("Hotkey.Toggle"));
	f->hotkey_toggle = obs_hotkey_register_source(parent, name.array, desc.array, motion_mask_hotkey_toggle, f);

	dstr_free(&name);
	dstr_free(&desc);
	f->hotkeys_registered = true;
}

static void motion_mask_tick(void *data, float seconds)
{
	struct motion_mask *f = data;
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (parent && !f->hotkeys_registered)
		register_hotkeys(f, parent);

	/* automatic trigger: play the entrance every time the source appears */
	bool showing = parent ? obs_source_showing(parent) : false;
	if (f->trigger == TRIGGER_ON_SHOW) {
		if (showing && !f->was_showing)
			start_anim(f, STATE_IN);
		else if (!showing && f->was_showing)
			f->state = STATE_HIDDEN;
	}
	f->was_showing = showing;

	/* hotkeys and test buttons */
	long cmd = os_atomic_set_long(&f->pending, CMD_NONE);
	if (cmd == CMD_TOGGLE)
		cmd = (f->state == STATE_SHOWN || f->state == STATE_IN) ? CMD_OUT : CMD_IN;
	if (cmd == CMD_IN)
		start_anim(f, STATE_IN);
	else if (cmd == CMD_OUT)
		start_anim(f, STATE_OUT);

	reset_motion(f);

	if (f->state == STATE_IN || f->state == STATE_OUT) {
		bool entering = f->state == STATE_IN;
		int anim = entering ? f->anim_in : f->anim_out;
		float delay = entering ? f->delay : 0.0f;
		float duration = anim == ANIM_NONE ? 0.0f : f->duration;

		f->elapsed += seconds;

		float t = duration > 0.0f ? (f->elapsed - delay) / duration : (f->elapsed >= delay ? 1.0f : 0.0f);

		if (t >= 1.0f) {
			f->state = entering ? STATE_SHOWN : STATE_HIDDEN;
		} else if (t <= 0.0f) {
			/* still waiting for the delay */
			f->draw_nothing = entering;
		} else {
			float v = entering ? apply_easing(f->easing, t) : apply_easing(f->easing, 1.0f - t);
			apply_anim(f, anim, v);
		}
	}

	if (f->state == STATE_HIDDEN)
		f->draw_nothing = true;
}

/* ------------------------------------------------------------------ */
/* render                                                              */

static void motion_mask_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct motion_mask *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);

	if (!f->effect || !target) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (f->draw_nothing)
		return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (!width || !height) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	int mask_type = f->mask_type;
	gs_texture_t *mask_tex = f->image_tex;
	if (mask_type == MASK_IMAGE && !mask_tex)
		mask_type = MASK_NONE;

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	struct vec2 source_size;
	vec2_set(&source_size, (float)width, (float)height);

	struct vec2 mask_size;
	vec2_set(&mask_size, f->mask_size.x * f->mask_scale, f->mask_size.y * f->mask_scale);

	float rad = f->grad_angle * (float)(M_PI / 180.0);
	struct vec2 grad_dir;
	vec2_set(&grad_dir, cosf(rad), sinf(rad));

	if (mask_tex)
		gs_effect_set_texture(f->p_mask_image, mask_tex);
	gs_effect_set_int(f->p_mask_type, mask_type);
	gs_effect_set_vec2(f->p_source_size, &source_size);
	gs_effect_set_vec2(f->p_mask_center, &f->mask_center);
	gs_effect_set_vec2(f->p_mask_size, &mask_size);
	gs_effect_set_vec4(f->p_corner_radii, &f->corner_radii);
	gs_effect_set_float(f->p_feather, f->feather);
	gs_effect_set_float(f->p_invert_mask, f->invert ? 1.0f : 0.0f);
	gs_effect_set_float(f->p_image_channel, (float)f->image_channel);
	gs_effect_set_vec2(f->p_grad_dir, &grad_dir);
	gs_effect_set_float(f->p_grad_pos, f->grad_pos);
	gs_effect_set_float(f->p_grad_width, f->grad_width);
	gs_effect_set_vec2(f->p_move_offset, &f->move_offset);
	gs_effect_set_float(f->p_move_scale, f->move_scale);
	gs_effect_set_float(f->p_opacity, f->opacity);
	gs_effect_set_float(f->p_wipe_enabled, f->wipe_enabled ? 1.0f : 0.0f);
	gs_effect_set_vec2(f->p_wipe_dir, &f->wipe_dir);
	gs_effect_set_float(f->p_wipe_progress, f->wipe_progress);
	gs_effect_set_float(f->p_wipe_soft, 0.08f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(f->context, f->effect, 0, 0);
	gs_blend_state_pop();
}

/* ------------------------------------------------------------------ */
/* settings                                                            */

static void unload_image(struct motion_mask *f)
{
	if (!f->image_tex)
		return;
	obs_enter_graphics();
	gs_texture_destroy(f->image_tex);
	obs_leave_graphics();
	f->image_tex = NULL;
}

static void load_image(struct motion_mask *f, const char *path)
{
	if (f->image_path && path && strcmp(f->image_path, path) == 0 && f->image_tex)
		return;

	unload_image(f);
	bfree(f->image_path);
	f->image_path = (path && *path) ? bstrdup(path) : NULL;
	if (!f->image_path)
		return;

	obs_enter_graphics();
	f->image_tex = gs_texture_create_from_file(f->image_path);
	obs_leave_graphics();

	if (!f->image_tex)
		blog(LOG_WARNING, "[ac-motion-mask] could not load mask image '%s'", f->image_path);
}

static void motion_mask_update(void *data, obs_data_t *settings)
{
	struct motion_mask *f = data;

	f->mask_type = (int)obs_data_get_int(settings, "mask_type");
	f->mask_size.x = (float)obs_data_get_double(settings, "mask_width") / 100.0f;
	f->mask_size.y = (float)obs_data_get_double(settings, "mask_height") / 100.0f;
	f->mask_center.x = (float)obs_data_get_double(settings, "mask_x") / 100.0f;
	f->mask_center.y = (float)obs_data_get_double(settings, "mask_y") / 100.0f;
	if (obs_data_get_bool(settings, "corner_individual")) {
		vec4_set(&f->corner_radii, (float)obs_data_get_double(settings, "corner_tl") / 100.0f,
			 (float)obs_data_get_double(settings, "corner_tr") / 100.0f,
			 (float)obs_data_get_double(settings, "corner_br") / 100.0f,
			 (float)obs_data_get_double(settings, "corner_bl") / 100.0f);
	} else {
		float r = (float)obs_data_get_double(settings, "corner_radius") / 100.0f;
		vec4_set(&f->corner_radii, r, r, r, r);
	}
	f->feather = (float)obs_data_get_double(settings, "feather");
	f->invert = obs_data_get_bool(settings, "invert");
	f->image_channel = (int)obs_data_get_int(settings, "image_channel");
	f->grad_angle = (float)obs_data_get_double(settings, "grad_angle");
	f->grad_pos = (float)obs_data_get_double(settings, "grad_pos") / 100.0f;
	f->grad_width = (float)obs_data_get_double(settings, "grad_width") / 100.0f;

	f->trigger = (int)obs_data_get_int(settings, "trigger");
	f->anim_in = (int)obs_data_get_int(settings, "anim_in");
	f->anim_out = (int)obs_data_get_int(settings, "anim_out");
	f->easing = (int)obs_data_get_int(settings, "easing");
	f->duration = (float)obs_data_get_int(settings, "duration") / 1000.0f;
	f->delay = (float)obs_data_get_int(settings, "delay") / 1000.0f;

	if (f->mask_type == MASK_IMAGE)
		load_image(f, obs_data_get_string(settings, "image_path"));
}

static void motion_mask_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "preset", 0);
	obs_data_set_default_int(settings, "mask_type", MASK_RECT);
	obs_data_set_default_double(settings, "mask_width", 100.0);
	obs_data_set_default_double(settings, "mask_height", 100.0);
	obs_data_set_default_double(settings, "mask_x", 50.0);
	obs_data_set_default_double(settings, "mask_y", 50.0);
	obs_data_set_default_double(settings, "corner_radius", 15.0);
	obs_data_set_default_bool(settings, "corner_individual", false);
	obs_data_set_default_double(settings, "corner_tl", 15.0);
	obs_data_set_default_double(settings, "corner_tr", 15.0);
	obs_data_set_default_double(settings, "corner_br", 15.0);
	obs_data_set_default_double(settings, "corner_bl", 15.0);
	obs_data_set_default_double(settings, "feather", 0.0);
	obs_data_set_default_bool(settings, "invert", false);
	obs_data_set_default_int(settings, "image_channel", 0);
	obs_data_set_default_double(settings, "grad_angle", 0.0);
	obs_data_set_default_double(settings, "grad_pos", 50.0);
	obs_data_set_default_double(settings, "grad_width", 30.0);

	obs_data_set_default_int(settings, "trigger", TRIGGER_ON_SHOW);
	obs_data_set_default_int(settings, "anim_in", ANIM_ZOOM);
	obs_data_set_default_int(settings, "anim_out", ANIM_FADE);
	obs_data_set_default_int(settings, "easing", EASE_BACK);
	obs_data_set_default_int(settings, "duration", 500);
	obs_data_set_default_int(settings, "delay", 0);
}

/* ------------------------------------------------------------------ */
/* properties                                                          */

static bool mask_type_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int type = (int)obs_data_get_int(settings, "mask_type");

	bool shape = type == MASK_RECT || type == MASK_CIRCLE || type == MASK_ELLIPSE;
	bool image = type == MASK_IMAGE;
	bool gradient = type == MASK_GRADIENT;
	bool box = shape || image;

	obs_property_set_visible(obs_properties_get(props, "mask_width"), box);
	obs_property_set_visible(obs_properties_get(props, "mask_height"), box && type != MASK_CIRCLE);
	obs_property_set_visible(obs_properties_get(props, "mask_x"), box);
	obs_property_set_visible(obs_properties_get(props, "mask_y"), box);
	bool individual = obs_data_get_bool(settings, "corner_individual");
	obs_property_set_visible(obs_properties_get(props, "corner_individual"), type == MASK_RECT);
	obs_property_set_visible(obs_properties_get(props, "corner_radius"), type == MASK_RECT && !individual);
	obs_property_set_visible(obs_properties_get(props, "corner_tl"), type == MASK_RECT && individual);
	obs_property_set_visible(obs_properties_get(props, "corner_tr"), type == MASK_RECT && individual);
	obs_property_set_visible(obs_properties_get(props, "corner_br"), type == MASK_RECT && individual);
	obs_property_set_visible(obs_properties_get(props, "corner_bl"), type == MASK_RECT && individual);
	obs_property_set_visible(obs_properties_get(props, "feather"), shape);
	obs_property_set_visible(obs_properties_get(props, "image_path"), image);
	obs_property_set_visible(obs_properties_get(props, "image_channel"), image);
	obs_property_set_visible(obs_properties_get(props, "grad_angle"), gradient);
	obs_property_set_visible(obs_properties_get(props, "grad_pos"), gradient);
	obs_property_set_visible(obs_properties_get(props, "grad_width"), gradient);
	obs_property_set_visible(obs_properties_get(props, "invert"), type != MASK_NONE);
	return true;
}

static bool anim_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool has_in = obs_data_get_int(settings, "anim_in") != ANIM_NONE;
	bool has_out = obs_data_get_int(settings, "anim_out") != ANIM_NONE;
	bool any = has_in || has_out;

	obs_property_set_visible(obs_properties_get(props, "easing"), any);
	obs_property_set_visible(obs_properties_get(props, "duration"), any);
	obs_property_set_visible(obs_properties_get(props, "delay"), has_in);
	obs_property_set_visible(obs_properties_get(props, "test_in"), has_in);
	obs_property_set_visible(obs_properties_get(props, "test_out"), has_out);
	obs_property_set_visible(obs_properties_get(props, "out_hint"), has_out);
	return true;
}

enum preset_type {
	PRESET_CHOOSE = 0,
	PRESET_ROUND_CAM,
	PRESET_CARD,
	PRESET_SOFT,
	PRESET_SLIDE,
	PRESET_WIPE,
	PRESET_BOUNCE,
};

static void set_preset(obs_data_t *settings, int mask, double width, double radius, int anim_in, int anim_out,
		       int easing, int duration)
{
	obs_data_set_int(settings, "mask_type", mask);
	obs_data_set_double(settings, "mask_width", width);
	obs_data_set_double(settings, "mask_height", width);
	obs_data_set_double(settings, "mask_x", 50.0);
	obs_data_set_double(settings, "mask_y", 50.0);
	obs_data_set_bool(settings, "corner_individual", false);
	obs_data_set_double(settings, "corner_radius", radius);
	obs_data_set_double(settings, "feather", 0.0);
	obs_data_set_bool(settings, "invert", false);
	obs_data_set_int(settings, "anim_in", anim_in);
	obs_data_set_int(settings, "anim_out", anim_out);
	obs_data_set_int(settings, "easing", easing);
	obs_data_set_int(settings, "duration", duration);
	obs_data_set_int(settings, "delay", 0);
}

static bool preset_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	int preset = (int)obs_data_get_int(settings, "preset");

	switch (preset) {
	case PRESET_ROUND_CAM:
		set_preset(settings, MASK_CIRCLE, 90.0, 15.0, ANIM_ZOOM, ANIM_ZOOM, EASE_BACK, 500);
		break;
	case PRESET_CARD:
		set_preset(settings, MASK_RECT, 100.0, 15.0, ANIM_MASK, ANIM_FADE, EASE_OUT, 450);
		break;
	case PRESET_SOFT:
		set_preset(settings, MASK_NONE, 100.0, 15.0, ANIM_FADE, ANIM_FADE, EASE_IN_OUT, 400);
		break;
	case PRESET_SLIDE:
		set_preset(settings, MASK_RECT, 100.0, 10.0, ANIM_SLIDE_LEFT, ANIM_SLIDE_LEFT, EASE_OUT, 500);
		break;
	case PRESET_WIPE:
		set_preset(settings, MASK_NONE, 100.0, 15.0, ANIM_WIPE_RIGHT, ANIM_WIPE_LEFT, EASE_IN_OUT, 600);
		break;
	case PRESET_BOUNCE:
		set_preset(settings, MASK_RECT, 100.0, 15.0, ANIM_ZOOM, ANIM_ZOOM, EASE_BOUNCE, 700);
		break;
	default:
		return false;
	}

	/* a preset is a one-shot action: go back to "choose" so reopening the
	 * dialog never overwrites manual tweaks */
	obs_data_set_int(settings, "preset", PRESET_CHOOSE);
	mask_type_changed(props, p, settings);
	anim_changed(props, p, settings);
	return true;
}

static bool test_in_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct motion_mask *f = data;
	os_atomic_set_long(&f->pending, CMD_IN);
	return false;
}

static bool test_out_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct motion_mask *f = data;
	os_atomic_set_long(&f->pending, CMD_OUT);
	return false;
}

static bool support_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	acm_open_support_dialog();
	return false;
}

static void add_anim_list(obs_properties_t *props, const char *name, const char *label)
{
	obs_property_t *p = obs_properties_add_list(props, name, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(p, anim_changed);
	obs_property_list_add_int(p, T_("Anim.None"), ANIM_NONE);
	obs_property_list_add_int(p, T_("Anim.Fade"), ANIM_FADE);
	obs_property_list_add_int(p, T_("Anim.Zoom"), ANIM_ZOOM);
	obs_property_list_add_int(p, T_("Anim.Mask"), ANIM_MASK);
	obs_property_list_add_int(p, T_("Anim.SlideLeft"), ANIM_SLIDE_LEFT);
	obs_property_list_add_int(p, T_("Anim.SlideRight"), ANIM_SLIDE_RIGHT);
	obs_property_list_add_int(p, T_("Anim.SlideUp"), ANIM_SLIDE_UP);
	obs_property_list_add_int(p, T_("Anim.SlideDown"), ANIM_SLIDE_DOWN);
	obs_property_list_add_int(p, T_("Anim.WipeRight"), ANIM_WIPE_RIGHT);
	obs_property_list_add_int(p, T_("Anim.WipeLeft"), ANIM_WIPE_LEFT);
	obs_property_list_add_int(p, T_("Anim.WipeDown"), ANIM_WIPE_DOWN);
	obs_property_list_add_int(p, T_("Anim.WipeUp"), ANIM_WIPE_UP);
}

static obs_properties_t *motion_mask_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	/* --- quick style --- */
	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Preset.Choose"), PRESET_CHOOSE);
	obs_property_list_add_int(p, T_("Preset.RoundCam"), PRESET_ROUND_CAM);
	obs_property_list_add_int(p, T_("Preset.Card"), PRESET_CARD);
	obs_property_list_add_int(p, T_("Preset.Soft"), PRESET_SOFT);
	obs_property_list_add_int(p, T_("Preset.Slide"), PRESET_SLIDE);
	obs_property_list_add_int(p, T_("Preset.Wipe"), PRESET_WIPE);
	obs_property_list_add_int(p, T_("Preset.Bounce"), PRESET_BOUNCE);
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_changed);

	/* --- mask --- */
	obs_properties_t *mask = obs_properties_create();

	p = obs_properties_add_list(mask, "mask_type", T_("Mask.Type"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Mask.None"), MASK_NONE);
	obs_property_list_add_int(p, T_("Mask.Rect"), MASK_RECT);
	obs_property_list_add_int(p, T_("Mask.Circle"), MASK_CIRCLE);
	obs_property_list_add_int(p, T_("Mask.Ellipse"), MASK_ELLIPSE);
	obs_property_list_add_int(p, T_("Mask.Image"), MASK_IMAGE);
	obs_property_list_add_int(p, T_("Mask.Gradient"), MASK_GRADIENT);
	obs_property_set_modified_callback(p, mask_type_changed);

	p = obs_properties_add_path(mask, "image_path", T_("Mask.ImagePath"), OBS_PATH_FILE,
				    "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;All files (*.*)", NULL);
	p = obs_properties_add_list(mask, "image_channel", T_("Mask.ImageChannel"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Mask.ChannelAlpha"), 0);
	obs_property_list_add_int(p, T_("Mask.ChannelLuma"), 1);
	obs_property_set_long_description(p, T_("Mask.ImageChannel.Tip"));

	p = obs_properties_add_float_slider(mask, "mask_width", T_("Mask.Width"), 1.0, 200.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "mask_height", T_("Mask.Height"), 1.0, 200.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "mask_x", T_("Mask.X"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "mask_y", T_("Mask.Y"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "corner_radius", T_("Mask.CornerRadius"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_bool(mask, "corner_individual", T_("Mask.CornerIndividual"));
	obs_property_set_modified_callback(p, mask_type_changed);
	p = obs_properties_add_float_slider(mask, "corner_tl", T_("Mask.CornerTL"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "corner_tr", T_("Mask.CornerTR"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "corner_bl", T_("Mask.CornerBL"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "corner_br", T_("Mask.CornerBR"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "feather", T_("Mask.Feather"), 0.0, 300.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(mask, "grad_angle", T_("Mask.GradAngle"), 0.0, 360.0, 1.0);
	obs_property_float_set_suffix(p, "°");
	p = obs_properties_add_float_slider(mask, "grad_pos", T_("Mask.GradPos"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(mask, "grad_width", T_("Mask.GradWidth"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");

	obs_properties_add_bool(mask, "invert", T_("Mask.Invert"));

	obs_properties_add_group(props, "mask_group", T_("Group.Mask"), OBS_GROUP_NORMAL, mask);

	/* --- animation --- */
	obs_properties_t *anim = obs_properties_create();

	add_anim_list(anim, "anim_in", T_("Anim.In"));
	add_anim_list(anim, "anim_out", T_("Anim.Out"));

	p = obs_properties_add_list(anim, "easing", T_("Anim.Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Easing.Linear"), EASE_LINEAR);
	obs_property_list_add_int(p, T_("Easing.Out"), EASE_OUT);
	obs_property_list_add_int(p, T_("Easing.InOut"), EASE_IN_OUT);
	obs_property_list_add_int(p, T_("Easing.Back"), EASE_BACK);
	obs_property_list_add_int(p, T_("Easing.Elastic"), EASE_ELASTIC);
	obs_property_list_add_int(p, T_("Easing.Bounce"), EASE_BOUNCE);

	p = obs_properties_add_int_slider(anim, "duration", T_("Anim.Duration"), 0, 5000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int_slider(anim, "delay", T_("Anim.Delay"), 0, 5000, 50);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(anim, "trigger", T_("Anim.Trigger"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Trigger.OnShow"), TRIGGER_ON_SHOW);
	obs_property_list_add_int(p, T_("Trigger.Manual"), TRIGGER_MANUAL);
	obs_property_set_long_description(p, T_("Trigger.Tip"));

	obs_properties_add_button2(anim, "test_in", T_("Anim.TestIn"), test_in_clicked, data);
	obs_properties_add_button2(anim, "test_out", T_("Anim.TestOut"), test_out_clicked, data);
	p = obs_properties_add_text(anim, "out_hint", T_("Anim.OutHint"), OBS_TEXT_INFO);

	obs_properties_add_group(props, "anim_group", T_("Group.Animation"), OBS_GROUP_NORMAL, anim);

	obs_properties_add_button2(props, "support", T_("Support.Button"), support_clicked, NULL);

	return props;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

static const char *motion_mask_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("FilterName");
}

static void motion_mask_destroy(void *data)
{
	struct motion_mask *f = data;

	if (f->hotkeys_registered) {
		obs_hotkey_unregister(f->hotkey_in);
		obs_hotkey_unregister(f->hotkey_out);
		obs_hotkey_unregister(f->hotkey_toggle);
	}

	unload_image(f);
	bfree(f->image_path);

	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
	}

	bfree(f);
}

static void *motion_mask_create(obs_data_t *settings, obs_source_t *context)
{
	struct motion_mask *f = bzalloc(sizeof(*f));
	f->context = context;
	f->hotkey_in = OBS_INVALID_HOTKEY_ID;
	f->hotkey_out = OBS_INVALID_HOTKEY_ID;
	f->hotkey_toggle = OBS_INVALID_HOTKEY_ID;
	f->state = STATE_SHOWN;
	reset_motion(f);

	char *effect_path = obs_module_file("effects/motion-mask.effect");
	char *error = NULL;

	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(effect_path, &error);
	obs_leave_graphics();

	if (!f->effect) {
		blog(LOG_ERROR, "[ac-motion-mask] could not load effect '%s': %s", effect_path ? effect_path : "(null)",
		     error ? error : "unknown error");
	} else {
		f->p_mask_image = gs_effect_get_param_by_name(f->effect, "mask_image");
		f->p_mask_type = gs_effect_get_param_by_name(f->effect, "mask_type");
		f->p_source_size = gs_effect_get_param_by_name(f->effect, "source_size");
		f->p_mask_center = gs_effect_get_param_by_name(f->effect, "mask_center");
		f->p_mask_size = gs_effect_get_param_by_name(f->effect, "mask_size");
		f->p_corner_radii = gs_effect_get_param_by_name(f->effect, "corner_radii");
		f->p_feather = gs_effect_get_param_by_name(f->effect, "feather");
		f->p_invert_mask = gs_effect_get_param_by_name(f->effect, "invert_mask");
		f->p_image_channel = gs_effect_get_param_by_name(f->effect, "image_channel");
		f->p_grad_dir = gs_effect_get_param_by_name(f->effect, "grad_dir");
		f->p_grad_pos = gs_effect_get_param_by_name(f->effect, "grad_pos");
		f->p_grad_width = gs_effect_get_param_by_name(f->effect, "grad_width");
		f->p_move_offset = gs_effect_get_param_by_name(f->effect, "move_offset");
		f->p_move_scale = gs_effect_get_param_by_name(f->effect, "move_scale");
		f->p_opacity = gs_effect_get_param_by_name(f->effect, "opacity");
		f->p_wipe_enabled = gs_effect_get_param_by_name(f->effect, "wipe_enabled");
		f->p_wipe_dir = gs_effect_get_param_by_name(f->effect, "wipe_dir");
		f->p_wipe_progress = gs_effect_get_param_by_name(f->effect, "wipe_progress");
		f->p_wipe_soft = gs_effect_get_param_by_name(f->effect, "wipe_soft");
	}

	bfree(effect_path);
	bfree(error);

	motion_mask_update(f, settings);
	return f;
}

struct obs_source_info motion_mask_filter_info = {
	.id = FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = motion_mask_name,
	.create = motion_mask_create,
	.destroy = motion_mask_destroy,
	.update = motion_mask_update,
	.get_defaults = motion_mask_defaults,
	.get_properties = motion_mask_properties,
	.video_tick = motion_mask_tick,
	.video_render = motion_mask_render,
};
