/*
AC Motion Mask - scene item motion engine
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACM_POSITIONS_KEY "ac_motion_positions"

enum acm_easing {
	ACM_EASE_LINEAR = 0,
	ACM_EASE_OUT,
	ACM_EASE_IN_OUT,
	ACM_EASE_BACK,
	ACM_EASE_ELASTIC,
	ACM_EASE_BOUNCE,
};

struct acm_transform {
	struct obs_transform_info info;
	struct obs_sceneitem_crop crop;
};

void acm_engine_init(void);
void acm_engine_free(void);

float acm_ease(int easing, float t);

void acm_transform_from_item(obs_sceneitem_t *item, struct acm_transform *out);
void acm_transform_to_data(const struct acm_transform *t, obs_data_t *data);
void acm_transform_from_data(obs_data_t *data, struct acm_transform *out);

/* Returns the item's saved positions (always non-NULL, caller releases). */
obs_data_array_t *acm_positions_get(obs_sceneitem_t *item);
void acm_positions_set(obs_sceneitem_t *item, obs_data_array_t *positions);
/* Index of the position with that name, or -1. */
int acm_positions_find(obs_data_array_t *positions, const char *name);

/* Animates the item to the target transform. Replaces any running animation
 * on the same item. duration <= 0 applies it instantly. */
void acm_animate(obs_sceneitem_t *item, const struct acm_transform *to, float duration, int easing);

#ifdef __cplusplus
}
#endif
