#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/render_types.h"
#include "pocketjs/ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pocketjs_rgb565_renderer pocketjs_rgb565_renderer_t;
typedef struct pocketjs_rgb565_target pocketjs_rgb565_target_t;

void pocketjs_rgb565_renderer_config_defaults(
    pocketjs_rgb565_renderer_config_t *config);
esp_err_t
pocketjs_rgb565_renderer_create(const pocketjs_rgb565_renderer_config_t *config,
                                pocketjs_rgb565_renderer_t **out_renderer);
void pocketjs_rgb565_renderer_destroy(pocketjs_rgb565_renderer_t *renderer);

esp_err_t pocketjs_rgb565_target_create(pocketjs_rgb565_target_t **out_target);
void pocketjs_rgb565_target_invalidate(pocketjs_rgb565_target_t *target);
void pocketjs_rgb565_target_destroy(pocketjs_rgb565_target_t *target);

esp_err_t pocketjs_rgb565_prepare(pocketjs_rgb565_renderer_t *renderer,
                                  pocketjs_rgb565_target_t *target,
                                  const pocketjs_ui_frame_view_t *frame,
                                  pocketjs_rgb565_damage_plan_t *out_plan);

/** pocketjs_rgb565_prepare that may also report retained pixels to move.
 * When `out_motion->present`, copy the target's pixels from the destination
 * rectangle offset by (-dx, -dy) into `out_motion->destination` before
 * rendering the plan's regions. Coordinates are logical; multiply by the
 * raster density. Source and destination may overlap. */
esp_err_t pocketjs_rgb565_prepare_motion(pocketjs_rgb565_renderer_t *renderer,
                                         pocketjs_rgb565_target_t *target,
                                         const pocketjs_ui_frame_view_t *frame,
                                         pocketjs_rgb565_damage_plan_t *out_plan,
                                         pocketjs_rgb565_motion_t *out_motion);

/** Render one logical damage rectangle into a full-viewport-width strip.
 * Pixels outside the rectangle's horizontal interval are left untouched.
 * Distinct renderers may render strips of one frame on different tasks at
 * the same time; the frame's core must not change until every strip returns. */
esp_err_t
pocketjs_rgb565_render_strip(pocketjs_rgb565_renderer_t *renderer,
                             const pocketjs_ui_frame_view_t *frame,
                             uint16_t *destination, size_t destination_pixels,
                             pocketjs_rgb565_rect_t region,
                             const pocketjs_rgb565_accelerator_t *accelerator,
                             pocketjs_rgb565_render_stats_t *out_stats);

esp_err_t pocketjs_rgb565_commit(pocketjs_rgb565_renderer_t *renderer,
                                 pocketjs_rgb565_target_t *target,
                                 const pocketjs_ui_frame_view_t *frame);

void pocketjs_rgb565_abort(pocketjs_rgb565_renderer_t *renderer,
                           pocketjs_rgb565_target_t *target);

#ifdef __cplusplus
}
#endif
