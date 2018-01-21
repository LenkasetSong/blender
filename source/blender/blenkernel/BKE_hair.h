/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __BKE_HAIR_H__
#define __BKE_HAIR_H__

/** \file blender/blenkernel/BKE_hair.h
 *  \ingroup bke
 */

#include "BLI_utildefines.h"

static const unsigned int HAIR_STRAND_INDEX_NONE = 0xFFFFFFFF;

struct HairFollicle;
struct HairPattern;
struct HairSystem;
struct HairDrawSettings;
struct DerivedMesh;
struct MeshSample;
struct Object;

/* Create a new hair system instance */
struct HairSystem* BKE_hair_new(void);
/* Copy an existing hair system */
struct HairSystem* BKE_hair_copy(struct HairSystem *hsys);
/* Delete a hair system */
void BKE_hair_free(struct HairSystem *hsys);

/* === Guide Strands === */

void BKE_hair_guide_curves_begin(struct HairSystem *hsys, int totcurves, int totverts);
void BKE_hair_set_guide_curve(struct HairSystem *hsys, int index, const struct MeshSample *mesh_sample, int numverts);
void BKE_hair_set_guide_vertex(struct HairSystem *hsys, int index, int flag, const float co[3]);
void BKE_hair_guide_curves_end(struct HairSystem *hsys);

/* === Follicles === */

/* Calculate surface area of a scalp mesh */
float BKE_hair_calc_surface_area(struct DerivedMesh *scalp);

/* Calculate a density value based on surface area and sample count */
float BKE_hair_calc_density_from_count(float area, int count);
/* Calculate maximum sample count based on surface area and density */
int BKE_hair_calc_max_count_from_density(float area, float density);

/* Calculate a density value based on a minimum distance */
float BKE_hair_calc_density_from_min_distance(float min_distance);
/* Calculate a minimum distance based on density */
float BKE_hair_calc_min_distance_from_density(float density);

/* Distribute hair follicles on a scalp mesh */
void BKE_hair_generate_follicles(
        struct HairSystem* hsys,
        struct DerivedMesh *scalp,
        unsigned int seed,
        int count);

void BKE_hair_bind_follicles(struct HairSystem *hsys, struct DerivedMesh *scalp);

/* === Draw Settings === */

struct HairDrawSettings* BKE_hair_draw_settings_new(void);
struct HairDrawSettings* BKE_hair_draw_settings_copy(struct HairDrawSettings *draw_settings);
void BKE_hair_draw_settings_free(struct HairDrawSettings *draw_settings);

/* === Draw Cache === */

enum {
	BKE_HAIR_BATCH_DIRTY_FIBERS = (1 << 0),
	BKE_HAIR_BATCH_DIRTY_STRANDS = (1 << 1),
	BKE_HAIR_BATCH_DIRTY_ALL = 0xFFFF,
};
void BKE_hair_batch_cache_dirty(struct HairSystem* hsys, int mode);
void BKE_hair_batch_cache_free(struct HairSystem* hsys);

int* BKE_hair_get_fiber_lengths(const struct HairSystem* hsys, int subdiv);
void BKE_hair_get_texture_buffer_size(
        const struct HairSystem* hsys,
        int subdiv,
        int *r_size,
        int *r_strand_map_start,
        int *r_strand_vertex_start,
        int *r_fiber_start);
void BKE_hair_get_texture_buffer(
        const struct HairSystem* hsys,
        struct DerivedMesh *scalp,
        int subdiv,
        void *texbuffer);

#endif