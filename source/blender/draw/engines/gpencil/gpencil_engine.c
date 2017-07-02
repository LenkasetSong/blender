/*
 * Copyright 2017, Blender Foundation.
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
 * Contributor(s): Antonio Vazquez
 *
 */

/** \file blender/draw/engines/gpencil/gpencil_engine.c
 *  \ingroup draw
 */

#include "DRW_engine.h"
#include "DRW_render.h"

#include "BKE_global.h"
#include "BKE_paint.h"

#include "DNA_gpencil_types.h"
#include "DNA_view3d_types.h"

#include "draw_mode_engines.h"

#include "gpencil_engine.h"

extern char datatoc_gpencil_fill_vert_glsl[];
extern char datatoc_gpencil_fill_frag_glsl[];
extern char datatoc_gpencil_stroke_vert_glsl[];
extern char datatoc_gpencil_stroke_geom_glsl[];
extern char datatoc_gpencil_stroke_frag_glsl[];
extern char datatoc_gpencil_zdepth_mix_frag_glsl[];
extern char datatoc_gpencil_point_vert_glsl[];
extern char datatoc_gpencil_point_frag_glsl[];

/* *********** STATIC *********** */
static GPENCIL_e_data e_data = {NULL}; /* Engine data */

/* *********** FUNCTIONS *********** */

static void GPENCIL_engine_init(void *vedata)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;

	const float *viewport_size = DRW_viewport_size_get();

	DRWFboTexture tex_color[2] = {{
			&e_data.temp_fbcolor_depth_tx, DRW_TEX_DEPTH_24, DRW_TEX_TEMP },
			{ &e_data.temp_fbcolor_color_tx, DRW_TEX_RGBA_16, DRW_TEX_TEMP }
	};
	/* init temp framebuffer */
	DRW_framebuffer_init(
		&fbl->temp_color_fb, &draw_engine_gpencil_type,
		(int)viewport_size[0], (int)viewport_size[1],
		tex_color, ARRAY_SIZE(tex_color));

	/* normal fill shader */
	if (!e_data.gpencil_fill_sh) {
		e_data.gpencil_fill_sh = DRW_shader_create(datatoc_gpencil_fill_vert_glsl, NULL,
			datatoc_gpencil_fill_frag_glsl,
			NULL);
	}

	/* normal stroke shader using geometry to display lines */
	if (!e_data.gpencil_stroke_sh) {
		e_data.gpencil_stroke_sh = DRW_shader_create(datatoc_gpencil_stroke_vert_glsl,
			datatoc_gpencil_stroke_geom_glsl,
			datatoc_gpencil_stroke_frag_glsl,
			NULL);
	}
	if (!e_data.gpencil_point_sh) {
		e_data.gpencil_point_sh = DRW_shader_create(datatoc_gpencil_point_vert_glsl,
			NULL,
			datatoc_gpencil_point_frag_glsl,
			NULL);
	}

	/* used for edit points or strokes with one point only */
	if (!e_data.gpencil_volumetric_sh) {
		e_data.gpencil_volumetric_sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_POINT_VARYING_SIZE_VARYING_COLOR);
	}

	/* used to filling during drawing */
	if (!e_data.gpencil_drawing_fill_sh) {
		e_data.gpencil_drawing_fill_sh = GPU_shader_get_builtin_shader(GPU_SHADER_3D_SMOOTH_COLOR);
	}

	if (!stl->storage) {
		stl->storage = MEM_callocN(sizeof(GPENCIL_Storage), "GPENCIL_Storage");
	}

	unit_m4(stl->storage->unit_matrix);

	/* blank texture used if no texture defined for fill shader */
	if (!e_data.gpencil_blank_texture) {
		e_data.gpencil_blank_texture = DRW_gpencil_create_blank_texture(16, 16);
	}

}

static void GPENCIL_engine_free(void)
{
	/* only free custom shaders, builtin shaders are freed in blender close */
	DRW_SHADER_FREE_SAFE(e_data.gpencil_fill_sh);
	DRW_SHADER_FREE_SAFE(e_data.gpencil_stroke_sh);
	DRW_SHADER_FREE_SAFE(e_data.gpencil_point_sh);
	DRW_SHADER_FREE_SAFE(e_data.gpencil_fullscreen_sh);
	DRW_TEXTURE_FREE_SAFE(e_data.gpencil_blank_texture);
}

static void GPENCIL_cache_init(void *vedata)
{
	if (G.debug_value == 668) {
		printf("GPENCIL_cache_init\n");
	}

	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;

	if (!stl->g_data) {
		/* Alloc transient pointers */
		stl->g_data = MEM_mallocN(sizeof(g_data), "g_data");
		stl->storage->xray = GP_XRAY_FRONT; /* used for drawing */
		stl->storage->stroke_style = STROKE_STYLE_SOLID; /* used for drawing */
	}
	if (!stl->shgroups) {
		/* Alloc maximum size because count strokes is very slow and can be very complex due onion skinning.
		   I tried to allocate only one block and using realloc, increasing the size when read a new strokes
		   in cache_finish, but the realloc produce weird things on screen, so we keep as is while we found
		   a better solution
		 */
		stl->shgroups = MEM_mallocN(sizeof(GPENCIL_shgroup) * GPENCIL_MAX_SHGROUPS, "GPENCIL_shgroup");
	}
	
	/* init gp objects cache */
	stl->g_data->gp_cache_used = 0;
	stl->g_data->gp_cache_size = 0;
	stl->g_data->gp_object_cache = NULL;

	/* full screen for mix zdepth*/
	if (!e_data.gpencil_fullscreen_sh) {
		e_data.gpencil_fullscreen_sh = DRW_shader_create_fullscreen(datatoc_gpencil_zdepth_mix_frag_glsl, NULL);
	}

	{
		/* Stroke pass */
		psl->stroke_pass = DRW_pass_create("Gpencil Stroke Pass", DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS | DRW_STATE_BLEND);
		stl->storage->shgroup_id = 0;

		/* edit pass */
		psl->edit_pass = DRW_pass_create("Gpencil Edit Pass", DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND);
		stl->g_data->shgrps_edit_volumetric = DRW_gpencil_shgroup_edit_volumetric_create(psl->edit_pass, e_data.gpencil_volumetric_sh);

		/* drawing buffer pass */
		const DRWContextState *draw_ctx = DRW_context_state_get();
		Palette *palette = BKE_palette_get_active_from_context(draw_ctx->evil_C);
		PaletteColor *palcolor = BKE_palette_color_get_active(palette);
		stl->storage->stroke_style = STROKE_STYLE_SOLID;


		psl->drawing_pass = DRW_pass_create("Gpencil Drawing Pass", DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND);
		stl->g_data->shgrps_drawing_stroke = DRW_gpencil_shgroup_stroke_create(&e_data, vedata, psl->drawing_pass, e_data.gpencil_stroke_sh, NULL, NULL, palcolor, -1);
		stl->g_data->shgrps_drawing_fill = DRW_gpencil_shgroup_drawing_fill_create(psl->drawing_pass, e_data.gpencil_drawing_fill_sh);

		/* we need a full screen pass to combine the result of zdepth */
		struct Gwn_Batch *quad = DRW_cache_fullscreen_quad_get();

		psl->mix_pass = DRW_pass_create("GPencil Mix Pass", DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS);
		DRWShadingGroup *mix_shgrp = DRW_shgroup_create(e_data.gpencil_fullscreen_sh, psl->mix_pass);
		DRW_shgroup_call_add(mix_shgrp, quad, NULL);
		DRW_shgroup_uniform_buffer(mix_shgrp, "strokeColor", &e_data.temp_fbcolor_color_tx);
		DRW_shgroup_uniform_buffer(mix_shgrp, "strokeDepth", &e_data.temp_fbcolor_depth_tx);
	}
}

static void GPENCIL_cache_populate(void *vedata, Object *ob)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	ToolSettings *ts = scene->toolsettings;

	/* object datablock (this is not draw now) */
	if (ob->type == OB_GPENCIL && ob->gpd) {
		if (G.debug_value == 668) {
			printf("GPENCIL_cache_populate: %s\n", ob->id.name);
		}
		/* allocate memory for saving gp objects */
		stl->g_data->gp_object_cache = gpencil_object_cache_allocate(stl->g_data->gp_object_cache, &stl->g_data->gp_cache_size, &stl->g_data->gp_cache_used);
		/* add for drawing later */
		gpencil_object_cache_add(stl->g_data->gp_object_cache, ob, &stl->g_data->gp_cache_used);
		
		/* draw current painting strokes */
		DRW_gpencil_populate_buffer_strokes(vedata, ts, ob->gpd);
	}
}

static void GPENCIL_cache_finish(void *vedata)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	ToolSettings *ts = scene->toolsettings;

	stl->g_data->gpd_in_cache = BLI_ghash_str_new("GP datablock");

	/* Draw all pending objects */
	if (stl->g_data->gp_cache_used > 0) {
		for (int i = 0; i < stl->g_data->gp_cache_used; ++i) {
			Object *ob = stl->g_data->gp_object_cache[i].ob;
			/* save init shading group */
			stl->g_data->gp_object_cache[i].init_grp = stl->storage->shgroup_id;
			
			/* add to hash to avoid duplicate geometry cache*/
			if (!BLI_ghash_lookup(stl->g_data->gpd_in_cache, ob->gpd->id.name)) {
				BLI_ghash_insert(stl->g_data->gpd_in_cache, ob->gpd->id.name, ob->gpd);
				if (ob->gpd->flag & (GP_DATA_STROKE_EDITMODE | GP_DATA_STROKE_SCULPTMODE)) {
					ob->gpd->flag |= GP_DATA_CACHE_IS_DIRTY;
					ob->gpd->flag &= ~GP_DATA_CACHE_REUSE;
				}
			}
			else {
				ob->gpd->flag &= ~GP_DATA_CACHE_IS_DIRTY;
				ob->gpd->flag |= GP_DATA_CACHE_REUSE;
			}

			/* fill shading groups */
			DRW_gpencil_populate_datablock(&e_data, vedata, scene, ob, ts, ob->gpd);

			/* save end shading group */
			stl->g_data->gp_object_cache[i].end_grp = stl->storage->shgroup_id - 1;
			if (G.debug_value == 668) {
				printf("GPENCIL_cache_finish: %s %d->%d\n", ob->id.name, 
					stl->g_data->gp_object_cache[i].init_grp, stl->g_data->gp_object_cache[i].end_grp);
			}
		}
	}

	/* free hash buffer */
	if (stl->g_data->gpd_in_cache) {
		BLI_ghash_free(stl->g_data->gpd_in_cache, NULL, NULL);
		stl->g_data->gpd_in_cache = NULL;
	}
	MEM_SAFE_FREE(stl->g_data->gpd_in_cache);
}

/* helper function to sort inverse gpencil objects using qsort */
static int gpencil_object_cache_compare_zdepth(const void *a1, const void *a2)
{
	const tGPencilObjectCache *ps1 = a1, *ps2 = a2;

	if (ps1->zdepth < ps2->zdepth) return 1;
	else if (ps1->zdepth > ps2->zdepth) return -1;

	return 0;
}

static void GPENCIL_draw_scene(void *vedata)
{
	GPENCIL_StorageList *stl = ((GPENCIL_Data *)vedata)->stl;

	GPENCIL_PassList *psl = ((GPENCIL_Data *)vedata)->psl;
	GPENCIL_FramebufferList *fbl = ((GPENCIL_Data *)vedata)->fbl;
	DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
	float clearcol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int init_grp, end_grp;

	/* Draw all pending objects */
	if (stl->g_data->gp_cache_used > 0) {

		/* sort by zdepth */
		qsort(stl->g_data->gp_object_cache, stl->g_data->gp_cache_used, 
			sizeof(tGPencilObjectCache), gpencil_object_cache_compare_zdepth);

		/* attach temp textures */
		DRW_framebuffer_texture_attach(fbl->temp_color_fb, e_data.temp_fbcolor_depth_tx, 0, 0);
		DRW_framebuffer_texture_attach(fbl->temp_color_fb, e_data.temp_fbcolor_color_tx, 0, 0);

		for (int i = 0; i < stl->g_data->gp_cache_used; ++i) {
			Object *ob = stl->g_data->gp_object_cache[i].ob;
			init_grp = stl->g_data->gp_object_cache[i].init_grp;
			end_grp = stl->g_data->gp_object_cache[i].end_grp;
			if (end_grp >= init_grp) {
				/* Render stroke in separated framebuffer */
				DRW_framebuffer_bind(fbl->temp_color_fb);
				DRW_framebuffer_clear(true, true, false, clearcol, 1.0f);

				/* Stroke Pass: DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND | DRW_STATE_WRITE_DEPTH
				 * draw only a subset that usually start with a fill and end with stroke because the
				 * shading groups are created by pairs */
				if (G.debug_value == 668) {
					printf("GPENCIL_draw_scene: %s %d->%d\n", ob->id.name, init_grp, end_grp);
				}

				DRW_draw_pass_subset(psl->stroke_pass,
					stl->shgroups[init_grp].shgrps_fill != NULL ? stl->shgroups[init_grp].shgrps_fill : stl->shgroups[init_grp].shgrps_stroke,
					stl->shgroups[end_grp].shgrps_stroke);

				/* Combine with scene buffer */
				DRW_framebuffer_bind(dfbl->default_fb);

				/* Mix Pass: DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS */
				DRW_draw_pass(psl->mix_pass);
			}
		}
		/* edit points */
		DRW_draw_pass(psl->edit_pass);

		/* detach temp textures */
		DRW_framebuffer_texture_detach(e_data.temp_fbcolor_depth_tx);
		DRW_framebuffer_texture_detach(e_data.temp_fbcolor_color_tx);
		
		/* attach again default framebuffer */
		DRW_framebuffer_bind(dfbl->default_fb);
	}
	/* free memory */
	MEM_SAFE_FREE(stl->g_data->gp_object_cache);

	/* current drawing buffer */
	DRW_draw_pass(psl->drawing_pass);
}

static const DrawEngineDataSize GPENCIL_data_size = DRW_VIEWPORT_DATA_SIZE(GPENCIL_Data);

DrawEngineType draw_engine_gpencil_type = {
	NULL, NULL,
	N_("GpencilMode"),
	&GPENCIL_data_size,
	&GPENCIL_engine_init,
	&GPENCIL_engine_free,
	&GPENCIL_cache_init,
	&GPENCIL_cache_populate,
	&GPENCIL_cache_finish,
	NULL,
	&GPENCIL_draw_scene
};