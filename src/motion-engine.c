/*
AC Motion Mask - scene item motion engine
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

#include "motion-engine.h"

#include <util/darray.h>
#include <util/threading.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct acm_anim {
	obs_sceneitem_t *item;
	struct acm_transform from;
	struct acm_transform to;
	float elapsed;
	float duration;
	int easing;
};

static pthread_mutex_t anim_mutex;
static DARRAY(struct acm_anim) anims;
static bool engine_ready = false;

/* ------------------------------------------------------------------ */
/* easing                                                              */

static float ease_bounce_out(float t)
{
	const float n1 = 7.5625f;
	const float d1 = 2.75f;

	if (t < 1.0f / d1)
		return n1 * t * t;
	if (t < 2.0f / d1) {
		t -= 1.5f / d1;
		return n1 * t * t + 0.75f;
	}
	if (t < 2.5f / d1) {
		t -= 2.25f / d1;
		return n1 * t * t + 0.9375f;
	}
	t -= 2.625f / d1;
	return n1 * t * t + 0.984375f;
}

float acm_ease(int easing, float t)
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	switch (easing) {
	case ACM_EASE_OUT: {
		float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case ACM_EASE_IN_OUT:
		if (t < 0.5f)
			return 4.0f * t * t * t;
		else {
			float u = -2.0f * t + 2.0f;
			return 1.0f - u * u * u / 2.0f;
		}
	case ACM_EASE_BACK: {
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	case ACM_EASE_ELASTIC: {
		const float c4 = (float)(2.0 * M_PI / 3.0);
		return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
	}
	case ACM_EASE_BOUNCE:
		return ease_bounce_out(t);
	case ACM_EASE_LINEAR:
	default:
		return t;
	}
}

/* ------------------------------------------------------------------ */
/* transforms                                                          */

void acm_transform_from_item(obs_sceneitem_t *item, struct acm_transform *out)
{
	memset(out, 0, sizeof(*out));
	obs_sceneitem_get_info2(item, &out->info);
	obs_sceneitem_get_crop(item, &out->crop);
}

static void set_vec2(obs_data_t *data, const char *name, const struct vec2 *v)
{
	obs_data_t *obj = obs_data_create();
	obs_data_set_double(obj, "x", v->x);
	obs_data_set_double(obj, "y", v->y);
	obs_data_set_obj(data, name, obj);
	obs_data_release(obj);
}

static void get_vec2(obs_data_t *data, const char *name, struct vec2 *v)
{
	obs_data_t *obj = obs_data_get_obj(data, name);
	if (!obj) {
		vec2_zero(v);
		return;
	}
	v->x = (float)obs_data_get_double(obj, "x");
	v->y = (float)obs_data_get_double(obj, "y");
	obs_data_release(obj);
}

void acm_transform_to_data(const struct acm_transform *t, obs_data_t *data)
{
	set_vec2(data, "pos", &t->info.pos);
	set_vec2(data, "scale", &t->info.scale);
	set_vec2(data, "bounds", &t->info.bounds);
	obs_data_set_double(data, "rot", t->info.rot);
	obs_data_set_int(data, "alignment", t->info.alignment);
	obs_data_set_int(data, "bounds_type", t->info.bounds_type);
	obs_data_set_int(data, "bounds_alignment", t->info.bounds_alignment);
	obs_data_set_bool(data, "crop_to_bounds", t->info.crop_to_bounds);
	obs_data_set_int(data, "crop_left", t->crop.left);
	obs_data_set_int(data, "crop_top", t->crop.top);
	obs_data_set_int(data, "crop_right", t->crop.right);
	obs_data_set_int(data, "crop_bottom", t->crop.bottom);
}

void acm_transform_from_data(obs_data_t *data, struct acm_transform *out)
{
	memset(out, 0, sizeof(*out));
	get_vec2(data, "pos", &out->info.pos);
	get_vec2(data, "scale", &out->info.scale);
	get_vec2(data, "bounds", &out->info.bounds);
	out->info.rot = (float)obs_data_get_double(data, "rot");
	out->info.alignment = (uint32_t)obs_data_get_int(data, "alignment");
	out->info.bounds_type = (enum obs_bounds_type)obs_data_get_int(data, "bounds_type");
	out->info.bounds_alignment = (uint32_t)obs_data_get_int(data, "bounds_alignment");
	out->info.crop_to_bounds = obs_data_get_bool(data, "crop_to_bounds");
	out->crop.left = (int)obs_data_get_int(data, "crop_left");
	out->crop.top = (int)obs_data_get_int(data, "crop_top");
	out->crop.right = (int)obs_data_get_int(data, "crop_right");
	out->crop.bottom = (int)obs_data_get_int(data, "crop_bottom");
}

obs_data_array_t *acm_positions_get(obs_sceneitem_t *item)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	obs_data_array_t *arr = obs_data_get_array(priv, ACM_POSITIONS_KEY);
	if (!arr) {
		arr = obs_data_array_create();
		obs_data_set_array(priv, ACM_POSITIONS_KEY, arr);
	}
	obs_data_release(priv);
	return arr;
}

void acm_positions_set(obs_sceneitem_t *item, obs_data_array_t *positions)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	obs_data_set_array(priv, ACM_POSITIONS_KEY, positions);
	obs_data_release(priv);
}

int acm_positions_find(obs_data_array_t *positions, const char *name)
{
	size_t count = obs_data_array_count(positions);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *pos = obs_data_array_item(positions, i);
		bool match = strcmp(obs_data_get_string(pos, "name"), name) == 0;
		obs_data_release(pos);
		if (match)
			return (int)i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* animation                                                           */

static inline float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static inline int lerp_crop(int a, int b, float t)
{
	float v = lerpf((float)a, (float)b, t);
	return v < 0.0f ? 0 : (int)(v + 0.5f);
}

static void apply_transform(obs_sceneitem_t *item, const struct acm_transform *t)
{
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &t->info);
	obs_sceneitem_set_crop(item, &t->crop);
	obs_sceneitem_defer_update_end(item);
}

static void interpolate(const struct acm_transform *a, const struct acm_transform *b, float v,
			struct acm_transform *out)
{
	*out = *b;
	out->info.pos.x = lerpf(a->info.pos.x, b->info.pos.x, v);
	out->info.pos.y = lerpf(a->info.pos.y, b->info.pos.y, v);
	out->info.scale.x = lerpf(a->info.scale.x, b->info.scale.x, v);
	out->info.scale.y = lerpf(a->info.scale.y, b->info.scale.y, v);
	out->info.bounds.x = fmaxf(lerpf(a->info.bounds.x, b->info.bounds.x, v), 1.0f);
	out->info.bounds.y = fmaxf(lerpf(a->info.bounds.y, b->info.bounds.y, v), 1.0f);
	out->info.rot = lerpf(a->info.rot, b->info.rot, v);
	out->crop.left = lerp_crop(a->crop.left, b->crop.left, v);
	out->crop.top = lerp_crop(a->crop.top, b->crop.top, v);
	out->crop.right = lerp_crop(a->crop.right, b->crop.right, v);
	out->crop.bottom = lerp_crop(a->crop.bottom, b->crop.bottom, v);
}

/* Position, scale, bounds and rotation only mean the same thing when the
 * anchor and bounds mode match; otherwise jump straight to the target. */
static bool can_interpolate(const struct acm_transform *a, const struct acm_transform *b)
{
	return a->info.alignment == b->info.alignment && a->info.bounds_type == b->info.bounds_type &&
	       a->info.bounds_alignment == b->info.bounds_alignment;
}

static void remove_anim(size_t idx)
{
	obs_sceneitem_release(anims.array[idx].item);
	da_erase(anims, idx);
}

static void engine_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);

	pthread_mutex_lock(&anim_mutex);
	for (size_t i = anims.num; i > 0; i--) {
		struct acm_anim *a = &anims.array[i - 1];

		if (!obs_sceneitem_get_scene(a->item)) {
			remove_anim(i - 1);
			continue;
		}

		a->elapsed += seconds;
		float t = a->duration > 0.0f ? a->elapsed / a->duration : 1.0f;

		if (t >= 1.0f) {
			apply_transform(a->item, &a->to);
			remove_anim(i - 1);
			continue;
		}

		struct acm_transform cur;
		interpolate(&a->from, &a->to, acm_ease(a->easing, t), &cur);
		apply_transform(a->item, &cur);
	}
	pthread_mutex_unlock(&anim_mutex);
}

void acm_animate(obs_sceneitem_t *item, const struct acm_transform *to, float duration, int easing)
{
	if (!item || !engine_ready)
		return;

	struct acm_transform from;
	acm_transform_from_item(item, &from);

	pthread_mutex_lock(&anim_mutex);

	for (size_t i = 0; i < anims.num; i++) {
		if (anims.array[i].item == item) {
			remove_anim(i);
			break;
		}
	}

	if (duration <= 0.0f || !can_interpolate(&from, to)) {
		apply_transform(item, to);
	} else {
		struct acm_anim *a = da_push_back_new(anims);
		obs_sceneitem_addref(item);
		a->item = item;
		a->from = from;
		a->to = *to;
		a->elapsed = 0.0f;
		a->duration = duration;
		a->easing = easing;
	}

	pthread_mutex_unlock(&anim_mutex);
}

void acm_engine_init(void)
{
	if (engine_ready)
		return;
	pthread_mutex_init(&anim_mutex, NULL);
	da_init(anims);
	obs_add_tick_callback(engine_tick, NULL);
	engine_ready = true;
}

void acm_engine_free(void)
{
	if (!engine_ready)
		return;
	obs_remove_tick_callback(engine_tick, NULL);

	pthread_mutex_lock(&anim_mutex);
	while (anims.num)
		remove_anim(anims.num - 1);
	da_free(anims);
	pthread_mutex_unlock(&anim_mutex);

	pthread_mutex_destroy(&anim_mutex);
	engine_ready = false;
}
