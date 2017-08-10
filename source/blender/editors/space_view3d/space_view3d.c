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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 *
 * 
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_view3d/space_view3d.c
 *  \ingroup spview3d
 */


#include <string.h>
#include <stdio.h>

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_depsgraph.h"
#include "BKE_icons.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "ED_space_api.h"
#include "ED_screen.h"

#include "GPU_compositing.h"
#include "GPU_framebuffer.h"
#include "GPU_material.h"

#include "BIF_gl.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "RNA_access.h"

#include "UI_resources.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

#include "DEG_depsgraph.h"

#include "view3d_intern.h"  /* own include */


#include "UI_interface.h"

static void header_register(ARegionType* );
static void view3d_buttons_register2(ARegionType* );
static void view3d_panel_transform(const bContext *C, Panel *pa);
static int view3d_panel_transform_poll(const bContext *C, PanelType *UNUSED(pt));
static void v3d_transform_butsR(uiLayout *layout, PointerRNA *ptr);
//static void v3d_editvertex_buts(uiLayout *layout, View3D *v3d, Object *ob, float lim);
//static void view3d_panel_vgroup(const bContext *C, Panel *pa);
//static int view3d_panel_vgroup_poll(const bContext *C, PanelType *UNUSED(pt));

static void v3d_header_butsR(uiLayout *layout, PointerRNA *ptr);
static void view3d_header(const bContext *C, Header *he);


/* ******************** manage regions ********************* */

ARegion *view3d_has_buttons_region(ScrArea *sa)
{
	ARegion *ar, *arnew;

	ar = BKE_area_find_region_type(sa, RGN_TYPE_UI);
	if (ar) return ar;
	
	/* add subdiv level; after header */
	ar = BKE_area_find_region_type(sa, RGN_TYPE_HEADER);

	/* is error! */
	if (ar == NULL) return NULL;
	
	arnew = MEM_callocN(sizeof(ARegion), "buttons for view3d");
	
	BLI_insertlinkafter(&sa->regionbase, ar, arnew);
	arnew->regiontype = RGN_TYPE_UI;
	arnew->alignment = RGN_ALIGN_RIGHT;
	
	arnew->flag = RGN_FLAG_HIDDEN;
	
	return arnew;
}

ARegion *view3d_has_tools_region(ScrArea *sa)
{
	ARegion *ar, *artool = NULL, *arprops = NULL, *arhead;
	
	for (ar = sa->regionbase.first; ar; ar = ar->next) {
		if (ar->regiontype == RGN_TYPE_TOOLS)
			artool = ar;
		if (ar->regiontype == RGN_TYPE_TOOL_PROPS)
			arprops = ar;
	}
	
	/* tool region hide/unhide also hides props */
	if (arprops && artool) return artool;
	
	if (artool == NULL) {
		/* add subdiv level; after header */
		for (arhead = sa->regionbase.first; arhead; arhead = arhead->next)
			if (arhead->regiontype == RGN_TYPE_HEADER)
				break;
		
		/* is error! */
		if (arhead == NULL) return NULL;
		
		artool = MEM_callocN(sizeof(ARegion), "tools for view3d");
		
		BLI_insertlinkafter(&sa->regionbase, arhead, artool);
		artool->regiontype = RGN_TYPE_TOOLS;
		artool->alignment = RGN_ALIGN_LEFT;
		artool->flag = RGN_FLAG_HIDDEN;
	}

	if (arprops == NULL) {
		/* add extra subdivided region for tool properties */
		arprops = MEM_callocN(sizeof(ARegion), "tool props for view3d");
		
		BLI_insertlinkafter(&sa->regionbase, artool, arprops);
		arprops->regiontype = RGN_TYPE_TOOL_PROPS;
		arprops->alignment = RGN_ALIGN_BOTTOM | RGN_SPLIT_PREV;
	}
	
	return artool;
}

/* ****************************************************** */

/* function to always find a regionview3d context inside 3D window */
RegionView3D *ED_view3d_context_rv3d(bContext *C)
{
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	
	if (rv3d == NULL) {
		ScrArea *sa = CTX_wm_area(C);
		if (sa && sa->spacetype == SPACE_VIEW3D) {
			ARegion *ar = BKE_area_find_region_active_win(sa);
			if (ar) {
				rv3d = ar->regiondata;
			}
		}
	}
	return rv3d;
}

/* ideally would return an rv3d but in some cases the region is needed too
 * so return that, the caller can then access the ar->regiondata */
bool ED_view3d_context_user_region(bContext *C, View3D **r_v3d, ARegion **r_ar)
{
	ScrArea *sa = CTX_wm_area(C);

	*r_v3d = NULL;
	*r_ar = NULL;

	if (sa && sa->spacetype == SPACE_VIEW3D) {
		ARegion *ar = CTX_wm_region(C);
		View3D *v3d = (View3D *)sa->spacedata.first;

		if (ar) {
			RegionView3D *rv3d;
			if ((ar->regiontype == RGN_TYPE_WINDOW) && (rv3d = ar->regiondata) && (rv3d->viewlock & RV3D_LOCKED) == 0) {
				*r_v3d = v3d;
				*r_ar = ar;
				return true;
			}
			else {
				ARegion *ar_unlock_user = NULL;
				ARegion *ar_unlock = NULL;
				for (ar = sa->regionbase.first; ar; ar = ar->next) {
					/* find the first unlocked rv3d */
					if (ar->regiondata && ar->regiontype == RGN_TYPE_WINDOW) {
						rv3d = ar->regiondata;
						if ((rv3d->viewlock & RV3D_LOCKED) == 0) {
							ar_unlock = ar;
							if (rv3d->persp == RV3D_PERSP || rv3d->persp == RV3D_CAMOB) {
								ar_unlock_user = ar;
								break;
							}
						}
					}
				}

				/* camera/perspective view get priority when the active region is locked */
				if (ar_unlock_user) {
					*r_v3d = v3d;
					*r_ar = ar_unlock_user;
					return true;
				}

				if (ar_unlock) {
					*r_v3d = v3d;
					*r_ar = ar_unlock;
					return true;
				}
			}
		}
	}

	return false;
}

/* Most of the time this isn't needed since you could assume the view matrix was
 * set while drawing, however when functions like mesh_foreachScreenVert are
 * called by selection tools, we can't be sure this object was the last.
 *
 * for example, transparent objects are drawn after editmode and will cause
 * the rv3d mat's to change and break selection.
 *
 * 'ED_view3d_init_mats_rv3d' should be called before
 * view3d_project_short_clip and view3d_project_short_noclip in cases where
 * these functions are not used during draw_object
 */
void ED_view3d_init_mats_rv3d(struct Object *ob, struct RegionView3D *rv3d)
{
	/* local viewmat and persmat, to calculate projections */
	mul_m4_m4m4(rv3d->viewmatob, rv3d->viewmat, ob->obmat);
	mul_m4_m4m4(rv3d->persmatob, rv3d->persmat, ob->obmat);

	/* initializes object space clipping, speeds up clip tests */
	ED_view3d_clipping_local(rv3d, ob->obmat);
}

void ED_view3d_init_mats_rv3d_gl(struct Object *ob, struct RegionView3D *rv3d)
{
	ED_view3d_init_mats_rv3d(ob, rv3d);

	/* we have to multiply instead of loading viewmatob to make
	 * it work with duplis using displists, otherwise it will
	 * override the dupli-matrix */
	glMultMatrixf(ob->obmat);
}

#ifdef DEBUG
/* ensure we correctly initialize */
void ED_view3d_clear_mats_rv3d(struct RegionView3D *rv3d)
{
	zero_m4(rv3d->viewmatob);
	zero_m4(rv3d->persmatob);
}

void ED_view3d_check_mats_rv3d(struct RegionView3D *rv3d)
{
	BLI_ASSERT_ZERO_M4(rv3d->viewmatob);
	BLI_ASSERT_ZERO_M4(rv3d->persmatob);
}
#endif

void ED_view3d_stop_render_preview(wmWindowManager *wm, ARegion *ar)
{
	RegionView3D *rv3d = ar->regiondata;

	if (rv3d->render_engine) {
#ifdef WITH_PYTHON
		BPy_BEGIN_ALLOW_THREADS;
#endif

		WM_jobs_kill_type(wm, ar, WM_JOB_TYPE_RENDER_PREVIEW);

#ifdef WITH_PYTHON
		BPy_END_ALLOW_THREADS;
#endif

		if (rv3d->render_engine->re)
			RE_Database_Free(rv3d->render_engine->re);
		RE_engine_free(rv3d->render_engine);
		rv3d->render_engine = NULL;
	}
}

void ED_view3d_shade_update(Main *bmain, Scene *scene, View3D *v3d, ScrArea *sa)
{
	wmWindowManager *wm = bmain->wm.first;

	if (v3d->drawtype != OB_RENDER) {
		ARegion *ar;

		for (ar = sa->regionbase.first; ar; ar = ar->next) {
			if (ar->regiondata)
				ED_view3d_stop_render_preview(wm, ar);
		}
	}
	else if (scene->obedit != NULL && scene->obedit->type == OB_MESH) {
		/* Tag mesh to load edit data. */
		DAG_id_tag_update(scene->obedit->data, 0);
	}
}

/* ******************** default callbacks for view3d space ***************** */

static SpaceLink *view3d_new(const bContext *C)
{
	Scene *scene = CTX_data_scene(C);
	ARegion *ar;
	View3D *v3d;
	RegionView3D *rv3d;
	
	v3d = MEM_callocN(sizeof(View3D), "initview3d");
	v3d->spacetype = SPACE_VIEW3D;
	v3d->blockscale = 0.7f;
	v3d->lay = v3d->layact = 1;
	if (scene) {
		v3d->lay = v3d->layact = scene->lay;
		v3d->camera = scene->camera;
	}
	v3d->scenelock = true;
	v3d->grid = 1.0f;
	v3d->gridlines = 16;
	v3d->gridsubdiv = 10;
	v3d->drawtype = OB_SOLID;

	v3d->gridflag = V3D_SHOW_X | V3D_SHOW_Y | V3D_SHOW_FLOOR;
	
	v3d->flag = V3D_SELECT_OUTLINE;
	v3d->flag2 = V3D_SHOW_RECONSTRUCTION | V3D_SHOW_GPENCIL;
	
	v3d->lens = 35.0f;
	v3d->near = 0.01f;
	v3d->far = 1000.0f;

	v3d->twflag |= U.tw_flag & V3D_USE_MANIPULATOR;
	v3d->twtype = V3D_MANIP_TRANSLATE;
	v3d->around = V3D_AROUND_CENTER_MEAN;
	
	v3d->bundle_size = 0.2f;
	v3d->bundle_drawtype = OB_PLAINAXES;

	/* stereo */
	v3d->stereo3d_camera = STEREO_3D_ID;
	v3d->stereo3d_flag |= V3D_S3D_DISPPLANE;
	v3d->stereo3d_convergence_alpha = 0.15f;
	v3d->stereo3d_volume_alpha = 0.05f;

	/* header */
	ar = MEM_callocN(sizeof(ARegion), "header for view3d");
	
	BLI_addtail(&v3d->regionbase, ar);
	ar->regiontype = RGN_TYPE_HEADER;
	ar->alignment = RGN_ALIGN_BOTTOM;
	
	/* tool shelf */
	ar = MEM_callocN(sizeof(ARegion), "toolshelf for view3d");
	
	BLI_addtail(&v3d->regionbase, ar);
	ar->regiontype = RGN_TYPE_TOOLS;
	ar->alignment = RGN_ALIGN_LEFT;
	ar->flag = RGN_FLAG_HIDDEN;
	
	/* tool properties */
	ar = MEM_callocN(sizeof(ARegion), "tool properties for view3d");
	
	BLI_addtail(&v3d->regionbase, ar);
	ar->regiontype = RGN_TYPE_TOOL_PROPS;
	ar->alignment = RGN_ALIGN_BOTTOM | RGN_SPLIT_PREV;
	ar->flag = RGN_FLAG_HIDDEN;
	
	/* buttons/list view */
	ar = MEM_callocN(sizeof(ARegion), "buttons for view3d");
	
	BLI_addtail(&v3d->regionbase, ar);
	ar->regiontype = RGN_TYPE_UI;
	ar->alignment = RGN_ALIGN_RIGHT;
	ar->flag = RGN_FLAG_HIDDEN;
	
	/* main region */
	ar = MEM_callocN(sizeof(ARegion), "main region for view3d");
	
	BLI_addtail(&v3d->regionbase, ar);
	ar->regiontype = RGN_TYPE_WINDOW;
	
	ar->regiondata = MEM_callocN(sizeof(RegionView3D), "region view3d");
	rv3d = ar->regiondata;
	rv3d->viewquat[0] = 1.0f;
	rv3d->persp = RV3D_PERSP;
	rv3d->view = RV3D_VIEW_USER;
	rv3d->dist = 10.0;
	
	return (SpaceLink *)v3d;
}

/* not spacelink itself */
static void view3d_free(SpaceLink *sl)
{
	View3D *vd = (View3D *) sl;
	BGpic *bgpic;

	for (bgpic = vd->bgpicbase.first; bgpic; bgpic = bgpic->next) {
		if (bgpic->source == V3D_BGPIC_IMAGE) {
			id_us_min((ID *)bgpic->ima);
		}
		else if (bgpic->source == V3D_BGPIC_MOVIE) {
			id_us_min((ID *)bgpic->clip);
		}
	}
	BLI_freelistN(&vd->bgpicbase);

	if (vd->localvd) MEM_freeN(vd->localvd);
	
	if (vd->properties_storage) MEM_freeN(vd->properties_storage);
	
	/* matcap material, its preview rect gets freed via icons */
	if (vd->defmaterial) {
		if (vd->defmaterial->gpumaterial.first)
			GPU_material_free(&vd->defmaterial->gpumaterial);
		BKE_previewimg_free(&vd->defmaterial->preview);
		MEM_freeN(vd->defmaterial);
	}

	if (vd->fx_settings.ssao)
		MEM_freeN(vd->fx_settings.ssao);
	if (vd->fx_settings.dof)
		MEM_freeN(vd->fx_settings.dof);
}


/* spacetype; init callback */
static void view3d_init(wmWindowManager *UNUSED(wm), ScrArea *UNUSED(sa))
{

}

static SpaceLink *view3d_duplicate(SpaceLink *sl)
{
	View3D *v3do = (View3D *)sl;
	View3D *v3dn = MEM_dupallocN(sl);
	BGpic *bgpic;
	
	/* clear or remove stuff from old */

	if (v3dn->localvd) {
		v3dn->localvd = NULL;
		v3dn->properties_storage = NULL;
		v3dn->lay = v3do->localvd->lay & 0xFFFFFF;
	}

	if (v3dn->drawtype == OB_RENDER)
		v3dn->drawtype = OB_SOLID;
	
	/* copy or clear inside new stuff */

	v3dn->defmaterial = NULL;

	BLI_duplicatelist(&v3dn->bgpicbase, &v3do->bgpicbase);
	for (bgpic = v3dn->bgpicbase.first; bgpic; bgpic = bgpic->next) {
		if (bgpic->source == V3D_BGPIC_IMAGE) {
			id_us_plus((ID *)bgpic->ima);
		}
		else if (bgpic->source == V3D_BGPIC_MOVIE) {
			id_us_plus((ID *)bgpic->clip);
		}
	}

	v3dn->properties_storage = NULL;
	if (v3dn->fx_settings.dof)
		v3dn->fx_settings.dof = MEM_dupallocN(v3do->fx_settings.dof);
	if (v3dn->fx_settings.ssao)
		v3dn->fx_settings.ssao = MEM_dupallocN(v3do->fx_settings.ssao);

	return (SpaceLink *)v3dn;
}

/* add handlers, stuff you only do once or on area/region changes */
static void view3d_main_region_init(wmWindowManager *wm, ARegion *ar)
{
	ListBase *lb;
	wmKeyMap *keymap;

	/* object ops. */
	
	/* important to be before Pose keymap since they can both be enabled at once */
	keymap = WM_keymap_find(wm->defaultconf, "Face Mask", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	
	keymap = WM_keymap_find(wm->defaultconf, "Weight Paint Vertex Selection", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	/* pose is not modal, operator poll checks for this */
	keymap = WM_keymap_find(wm->defaultconf, "Pose", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	keymap = WM_keymap_find(wm->defaultconf, "Object Mode", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Paint Curve", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Curve", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Image Paint", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Vertex Paint", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Weight Paint", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Sculpt", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	keymap = WM_keymap_find(wm->defaultconf, "Mesh", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	keymap = WM_keymap_find(wm->defaultconf, "Curve", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	keymap = WM_keymap_find(wm->defaultconf, "Armature", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Pose", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Metaball", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	keymap = WM_keymap_find(wm->defaultconf, "Lattice", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Particle", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	/* editfont keymap swallows all... */
	keymap = WM_keymap_find(wm->defaultconf, "Font", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Object Non-modal", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "Frames", 0, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	/* own keymap, last so modes can override it */
	keymap = WM_keymap_find(wm->defaultconf, "3D View Generic", SPACE_VIEW3D, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	keymap = WM_keymap_find(wm->defaultconf, "3D View", SPACE_VIEW3D, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
	
	/* add drop boxes */
	lb = WM_dropboxmap_find("View3D", SPACE_VIEW3D, RGN_TYPE_WINDOW);
	
	WM_event_add_dropbox_handler(&ar->handlers, lb);
	
}

static void view3d_main_region_exit(wmWindowManager *wm, ARegion *ar)
{
	RegionView3D *rv3d = ar->regiondata;

	ED_view3d_stop_render_preview(wm, ar);

	if (rv3d->gpuoffscreen) {
		GPU_offscreen_free(rv3d->gpuoffscreen);
		rv3d->gpuoffscreen = NULL;
	}
	
	if (rv3d->compositor) {
		GPU_fx_compositor_destroy(rv3d->compositor);
		rv3d->compositor = NULL;
	}
}

static int view3d_ob_drop_poll(bContext *UNUSED(C), wmDrag *drag, const wmEvent *UNUSED(event))
{
	if (drag->type == WM_DRAG_ID) {
		ID *id = drag->poin;
		if (GS(id->name) == ID_OB)
			return 1;
	}
	return 0;
}

static int view3d_group_drop_poll(bContext *UNUSED(C), wmDrag *drag, const wmEvent *UNUSED(event))
{
	if (drag->type == WM_DRAG_ID) {
		ID *id = drag->poin;
		if (GS(id->name) == ID_GR)
			return 1;
	}
	return 0;
}

static int view3d_mat_drop_poll(bContext *UNUSED(C), wmDrag *drag, const wmEvent *UNUSED(event))
{
	if (drag->type == WM_DRAG_ID) {
		ID *id = drag->poin;
		if (GS(id->name) == ID_MA)
			return 1;
	}
	return 0;
}

static int view3d_ima_drop_poll(bContext *UNUSED(C), wmDrag *drag, const wmEvent *UNUSED(event))
{
	if (drag->type == WM_DRAG_ID) {
		ID *id = drag->poin;
		if (GS(id->name) == ID_IM)
			return 1;
	}
	else if (drag->type == WM_DRAG_PATH) {
		if (ELEM(drag->icon, 0, ICON_FILE_IMAGE, ICON_FILE_MOVIE))   /* rule might not work? */
			return 1;
	}
	return 0;
}

static int view3d_ima_bg_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
	if (event->ctrl)
		return false;

	if (!ED_view3d_give_base_under_cursor(C, event->mval)) {
		return view3d_ima_drop_poll(C, drag, event);
	}
	return 0;
}

static int view3d_ima_empty_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
	Base *base = ED_view3d_give_base_under_cursor(C, event->mval);

	/* either holding and ctrl and no object, or dropping to empty */
	if (((base == NULL) && event->ctrl) ||
	    ((base != NULL) && base->object->type == OB_EMPTY))
	{
		return view3d_ima_drop_poll(C, drag, event);
	}

	return 0;
}

static int view3d_ima_mesh_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
	Base *base = ED_view3d_give_base_under_cursor(C, event->mval);

	if (base && base->object->type == OB_MESH)
		return view3d_ima_drop_poll(C, drag, event);
	return 0;
}

static void view3d_ob_drop_copy(wmDrag *drag, wmDropBox *drop)
{
	ID *id = drag->poin;

	RNA_string_set(drop->ptr, "name", id->name + 2);
}

static void view3d_group_drop_copy(wmDrag *drag, wmDropBox *drop)
{
	ID *id = drag->poin;
	
	drop->opcontext = WM_OP_EXEC_DEFAULT;
	RNA_string_set(drop->ptr, "name", id->name + 2);
}

static void view3d_id_drop_copy(wmDrag *drag, wmDropBox *drop)
{
	ID *id = drag->poin;
	
	RNA_string_set(drop->ptr, "name", id->name + 2);
}

static void view3d_id_path_drop_copy(wmDrag *drag, wmDropBox *drop)
{
	ID *id = drag->poin;
	
	if (id) {
		RNA_string_set(drop->ptr, "name", id->name + 2);
		RNA_struct_property_unset(drop->ptr, "filepath");
	}
	else if (drag->path[0]) {
		RNA_string_set(drop->ptr, "filepath", drag->path);
		RNA_struct_property_unset(drop->ptr, "image");
	}
}


/* region dropbox definition */
static void view3d_dropboxes(void)
{
	ListBase *lb = WM_dropboxmap_find("View3D", SPACE_VIEW3D, RGN_TYPE_WINDOW);
	
	WM_dropbox_add(lb, "OBJECT_OT_add_named", view3d_ob_drop_poll, view3d_ob_drop_copy);
	WM_dropbox_add(lb, "OBJECT_OT_drop_named_material", view3d_mat_drop_poll, view3d_id_drop_copy);
	WM_dropbox_add(lb, "MESH_OT_drop_named_image", view3d_ima_mesh_drop_poll, view3d_id_path_drop_copy);
	WM_dropbox_add(lb, "OBJECT_OT_drop_named_image", view3d_ima_empty_drop_poll, view3d_id_path_drop_copy);
	WM_dropbox_add(lb, "VIEW3D_OT_background_image_add", view3d_ima_bg_drop_poll, view3d_id_path_drop_copy);
	WM_dropbox_add(lb, "OBJECT_OT_group_instance_add", view3d_group_drop_poll, view3d_group_drop_copy);	
}



/* type callback, not region itself */
static void view3d_main_region_free(ARegion *ar)
{
	RegionView3D *rv3d = ar->regiondata;
	
	if (rv3d) {
		if (rv3d->localvd) MEM_freeN(rv3d->localvd);
		if (rv3d->clipbb) MEM_freeN(rv3d->clipbb);

		if (rv3d->render_engine)
			RE_engine_free(rv3d->render_engine);
		
		if (rv3d->depths) {
			if (rv3d->depths->depths) MEM_freeN(rv3d->depths->depths);
			MEM_freeN(rv3d->depths);
		}
		if (rv3d->sms) {
			MEM_freeN(rv3d->sms);
		}
		if (rv3d->gpuoffscreen) {
			GPU_offscreen_free(rv3d->gpuoffscreen);
		}
		if (rv3d->compositor) {
			GPU_fx_compositor_destroy(rv3d->compositor);
		}

		MEM_freeN(rv3d);
		ar->regiondata = NULL;
	}
}

/* copy regiondata */
static void *view3d_main_region_duplicate(void *poin)
{
	if (poin) {
		RegionView3D *rv3d = poin, *new;
	
		new = MEM_dupallocN(rv3d);
		if (rv3d->localvd)
			new->localvd = MEM_dupallocN(rv3d->localvd);
		if (rv3d->clipbb)
			new->clipbb = MEM_dupallocN(rv3d->clipbb);
		
		new->depths = NULL;
		new->gpuoffscreen = NULL;
		new->render_engine = NULL;
		new->sms = NULL;
		new->smooth_timer = NULL;
		new->compositor = NULL;
		
		return new;
	}
	return NULL;
}

static void view3d_recalc_used_layers(ARegion *ar, wmNotifier *wmn, Scene *scene)
{
	wmWindow *win = wmn->wm->winactive;
	ScrArea *sa;
	unsigned int lay_used = 0;
	Base *base;

	if (!win) return;

	base = scene->base.first;
	while (base) {
		lay_used |= base->lay & ((1 << 20) - 1); /* ignore localview */

		if (lay_used == (1 << 20) - 1)
			break;

		base = base->next;
	}

	for (sa = win->screen->areabase.first; sa; sa = sa->next) {
		if (sa->spacetype == SPACE_VIEW3D) {
			if (BLI_findindex(&sa->regionbase, ar) != -1) {
				View3D *v3d = sa->spacedata.first;
				v3d->lay_used = lay_used;
				break;
			}
		}
	}
}

static void view3d_main_region_listener(bScreen *sc, ScrArea *sa, ARegion *ar, wmNotifier *wmn)
{
	Scene *scene = sc->scene;
	View3D *v3d = sa->spacedata.first;
	
	/* context changes */
	switch (wmn->category) {
		case NC_ANIMATION:
			switch (wmn->data) {
				case ND_KEYFRAME_PROP:
				case ND_NLA_ACTCHANGE:
					ED_region_tag_redraw(ar);
					break;
				case ND_NLA:
				case ND_KEYFRAME:
					if (ELEM(wmn->action, NA_EDITED, NA_ADDED, NA_REMOVED))
						ED_region_tag_redraw(ar);
					break;
				case ND_ANIMCHAN:
					if (wmn->action == NA_SELECTED)
						ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_SCENE:
			switch (wmn->data) {
				case ND_LAYER_CONTENT:
					if (wmn->reference)
						view3d_recalc_used_layers(ar, wmn, wmn->reference);
					ED_region_tag_redraw(ar);
					break;
				case ND_FRAME:
				case ND_TRANSFORM:
				case ND_OB_ACTIVE:
				case ND_OB_SELECT:
				case ND_OB_VISIBLE:
				case ND_LAYER:
				case ND_RENDER_OPTIONS:
				case ND_MARKERS:
				case ND_MODE:
					ED_region_tag_redraw(ar);
					break;
				case ND_WORLD:
					/* handled by space_view3d_listener() for v3d access */
					break;
				case ND_DRAW_RENDER_VIEWPORT:
				{
					if (v3d->camera && (scene == wmn->reference)) {
						RegionView3D *rv3d = ar->regiondata;
						if (rv3d->persp == RV3D_CAMOB) {
							ED_region_tag_redraw(ar);
						}
					}
					break;
				}
			}
			if (wmn->action == NA_EDITED)
				ED_region_tag_redraw(ar);
			break;
		case NC_OBJECT:
			switch (wmn->data) {
				case ND_BONE_ACTIVE:
				case ND_BONE_SELECT:
				case ND_TRANSFORM:
				case ND_POSE:
				case ND_DRAW:
				case ND_MODIFIER:
				case ND_CONSTRAINT:
				case ND_KEYS:
				case ND_PARTICLE:
				case ND_POINTCACHE:
				case ND_LOD:
					ED_region_tag_redraw(ar);
					break;
			}
			switch (wmn->action) {
				case NA_ADDED:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_GEOM:
			switch (wmn->data) {
				case ND_DATA:
				case ND_VERTEX_GROUP:
				case ND_SELECT:
					ED_region_tag_redraw(ar);
					break;
			}
			switch (wmn->action) {
				case NA_EDITED:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_CAMERA:
			switch (wmn->data) {
				case ND_DRAW_RENDER_VIEWPORT:
				{
					if (v3d->camera && (v3d->camera->data == wmn->reference)) {
						RegionView3D *rv3d = ar->regiondata;
						if (rv3d->persp == RV3D_CAMOB) {
							ED_region_tag_redraw(ar);
						}
					}
					break;
				}
			}
			break;
		case NC_GROUP:
			/* all group ops for now */
			ED_region_tag_redraw(ar);
			break;
		case NC_BRUSH:
			switch (wmn->action) {
				case NA_EDITED:
					ED_region_tag_redraw_overlay(ar);
					break;
				case NA_SELECTED:
					/* used on brush changes - needed because 3d cursor
					 * has to be drawn if clone brush is selected */
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_MATERIAL:
			switch (wmn->data) {
				case ND_SHADING:
				case ND_NODES:
				{
#ifdef WITH_LEGACY_DEPSGRAPH
					Object *ob = OBACT;
					if ((v3d->drawtype == OB_MATERIAL) ||
					    (ob && (ob->mode == OB_MODE_TEXTURE_PAINT)) ||
					    (v3d->drawtype == OB_TEXTURE &&
					     (scene->gm.matmode == GAME_MAT_GLSL ||
					      BKE_scene_use_new_shading_nodes(scene))) ||
					    !DEG_depsgraph_use_legacy())
#endif
					{
						ED_region_tag_redraw(ar);
					}
					break;
				}
				case ND_SHADING_DRAW:
				case ND_SHADING_LINKS:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_WORLD:
			switch (wmn->data) {
				case ND_WORLD_DRAW:
					/* handled by space_view3d_listener() for v3d access */
					break;
			}
			break;
		case NC_LAMP:
			switch (wmn->data) {
				case ND_LIGHTING:
					if ((v3d->drawtype == OB_MATERIAL) ||
					    (v3d->drawtype == OB_TEXTURE && (scene->gm.matmode == GAME_MAT_GLSL)) ||
					    !DEG_depsgraph_use_legacy())
					{
						ED_region_tag_redraw(ar);
					}
					break;
				case ND_LIGHTING_DRAW:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_IMAGE:
			/* this could be more fine grained checks if we had
			 * more context than just the region */
			ED_region_tag_redraw(ar);
			break;
		case NC_TEXTURE:
			/* same as above */
			ED_region_tag_redraw(ar);
			break;
		case NC_MOVIECLIP:
			if (wmn->data == ND_DISPLAY || wmn->action == NA_EDITED)
				ED_region_tag_redraw(ar);
			break;
		case NC_SPACE:
			if (wmn->data == ND_SPACE_VIEW3D) {
				if (wmn->subtype == NS_VIEW3D_GPU) {
					RegionView3D *rv3d = ar->regiondata;
					rv3d->rflag |= RV3D_GPULIGHT_UPDATE;
				}
				ED_region_tag_redraw(ar);
			}
			break;
		case NC_ID:
			if (wmn->action == NA_RENAME)
				ED_region_tag_redraw(ar);
			break;
		case NC_SCREEN:
			switch (wmn->data) {
				case ND_ANIMPLAY:
				case ND_SKETCH:
					ED_region_tag_redraw(ar);
					break;
				case ND_SCREENBROWSE:
				case ND_SCREENDELETE:
				case ND_SCREENSET:
					/* screen was changed, need to update used layers due to NC_SCENE|ND_LAYER_CONTENT */
					/* updates used layers only for View3D in active screen */
					if (wmn->reference) {
						bScreen *sc_ref = wmn->reference;
						view3d_recalc_used_layers(ar, wmn, sc_ref->scene);
					}
					ED_region_tag_redraw(ar);
					break;
			}

			break;
		case NC_GPENCIL:
			if (wmn->data == ND_DATA || ELEM(wmn->action, NA_EDITED, NA_SELECTED)) {
				ED_region_tag_redraw(ar);
			}
			break;
	}
}

/* concept is to retrieve cursor type context-less */
static void view3d_main_region_cursor(wmWindow *win, ScrArea *UNUSED(sa), ARegion *UNUSED(ar))
{
	Scene *scene = win->screen->scene;

	if (scene->obedit) {
		WM_cursor_set(win, CURSOR_EDIT);
	}
	else {
		WM_cursor_set(win, CURSOR_STD);
	}
}

/* add handlers, stuff you only do once or on area/region changes */
static void view3d_header_region_init(wmWindowManager *wm, ARegion *ar)
{
	wmKeyMap *keymap = WM_keymap_find(wm->defaultconf, "3D View Generic", SPACE_VIEW3D, 0);
	
	WM_event_add_keymap_handler(&ar->handlers, keymap);

	ED_region_header_init(ar);
}

static void view3d_header_region_draw(const bContext *C, ARegion *ar)
{
	ED_region_header(C, ar);
}

static void view3d_header_region_listener(bScreen *UNUSED(sc), ScrArea *UNUSED(sa), ARegion *ar, wmNotifier *wmn)
{
	/* context changes */
	switch (wmn->category) {
		case NC_SCENE:
			switch (wmn->data) {
				case ND_FRAME:
				case ND_OB_ACTIVE:
				case ND_OB_SELECT:
				case ND_OB_VISIBLE:
				case ND_MODE:
				case ND_LAYER:
				case ND_TOOLSETTINGS:
				case ND_LAYER_CONTENT:
				case ND_RENDER_OPTIONS:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_SPACE:
			if (wmn->data == ND_SPACE_VIEW3D)
				ED_region_tag_redraw(ar);
			break;
		case NC_GPENCIL:
			if (wmn->data & ND_GPENCIL_EDITMODE)
				ED_region_tag_redraw(ar);
			break;
	}
}

/* add handlers, stuff you only do once or on area/region changes */
static void view3d_buttons_region_init(wmWindowManager *wm, ARegion *ar)
{
	wmKeyMap *keymap;

	ED_region_panels_init(wm, ar);
	
	keymap = WM_keymap_find(wm->defaultconf, "3D View Generic", SPACE_VIEW3D, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
}

static void view3d_buttons_region_draw(const bContext *C, ARegion *ar)
{
	ED_region_panels(C, ar, NULL, -1, true);
}

static void view3d_buttons_region_listener(bScreen *UNUSED(sc), ScrArea *UNUSED(sa), ARegion *ar, wmNotifier *wmn)
{
	/* context changes */
	switch (wmn->category) {
		case NC_ANIMATION:
			switch (wmn->data) {
				case ND_KEYFRAME_PROP:
				case ND_NLA_ACTCHANGE:
					ED_region_tag_redraw(ar);
					break;
				case ND_NLA:
				case ND_KEYFRAME:
					if (ELEM(wmn->action, NA_EDITED, NA_ADDED, NA_REMOVED))
						ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_SCENE:
			switch (wmn->data) {
				case ND_FRAME:
				case ND_OB_ACTIVE:
				case ND_OB_SELECT:
				case ND_OB_VISIBLE:
				case ND_MODE:
				case ND_LAYER:
				case ND_LAYER_CONTENT:
				case ND_TOOLSETTINGS:
					ED_region_tag_redraw(ar);
					break;
			}
			switch (wmn->action) {
				case NA_EDITED:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_OBJECT:
			switch (wmn->data) {
				case ND_BONE_ACTIVE:
				case ND_BONE_SELECT:
				case ND_TRANSFORM:
				case ND_POSE:
				case ND_DRAW:
				case ND_KEYS:
				case ND_MODIFIER:
					ED_region_tag_redraw(ar);
					break;
			}
			break;
		case NC_GEOM:
			switch (wmn->data) {
				case ND_DATA:
				case ND_VERTEX_GROUP:
				case ND_SELECT:
					ED_region_tag_redraw(ar);
					break;
			}
			if (wmn->action == NA_EDITED)
				ED_region_tag_redraw(ar);
			break;
		case NC_TEXTURE:
		case NC_MATERIAL:
			/* for brush textures */
			ED_region_tag_redraw(ar);
			break;
		case NC_BRUSH:
			/* NA_SELECTED is used on brush changes */
			if (ELEM(wmn->action, NA_EDITED, NA_SELECTED))
				ED_region_tag_redraw(ar);
			break;
		case NC_SPACE:
			if (wmn->data == ND_SPACE_VIEW3D)
				ED_region_tag_redraw(ar);
			break;
		case NC_ID:
			if (wmn->action == NA_RENAME)
				ED_region_tag_redraw(ar);
			break;
		case NC_GPENCIL:
			if ((wmn->data & (ND_DATA | ND_GPENCIL_EDITMODE)) || (wmn->action == NA_EDITED))
				ED_region_tag_redraw(ar);
			break;
		case NC_IMAGE:
			/* Update for the image layers in texture paint. */
			if (wmn->action == NA_EDITED)
				ED_region_tag_redraw(ar);
			break;
	}
}

/* add handlers, stuff you only do once or on area/region changes */
static void view3d_tools_region_init(wmWindowManager *wm, ARegion *ar)
{
	wmKeyMap *keymap;
	
	ED_region_panels_init(wm, ar);

	keymap = WM_keymap_find(wm->defaultconf, "3D View Generic", SPACE_VIEW3D, 0);
	WM_event_add_keymap_handler(&ar->handlers, keymap);
}

static void view3d_tools_region_draw(const bContext *C, ARegion *ar)
{
	ED_region_panels(C, ar, CTX_data_mode_string(C), -1, true);
}

static void view3d_props_region_listener(bScreen *UNUSED(sc), ScrArea *UNUSED(sa), ARegion *ar, wmNotifier *wmn)
{
	/* context changes */
	switch (wmn->category) {
		case NC_WM:
			if (wmn->data == ND_HISTORY)
				ED_region_tag_redraw(ar);
			break;
		case NC_SCENE:
			if (wmn->data == ND_MODE)
				ED_region_tag_redraw(ar);
			break;
		case NC_SPACE:
			if (wmn->data == ND_SPACE_VIEW3D)
				ED_region_tag_redraw(ar);
			break;
	}
}

/* area (not region) level listener */
static void space_view3d_listener(bScreen *UNUSED(sc), ScrArea *sa, struct wmNotifier *wmn)
{
	View3D *v3d = sa->spacedata.first;

	/* context changes */
	switch (wmn->category) {
		case NC_SCENE:
			switch (wmn->data) {
				case ND_WORLD:
					if (v3d->flag2 & V3D_RENDER_OVERRIDE)
						ED_area_tag_redraw_regiontype(sa, RGN_TYPE_WINDOW);
					break;
			}
			break;
		case NC_WORLD:
			switch (wmn->data) {
				case ND_WORLD_DRAW:
				case ND_WORLD:
					if (v3d->flag3 & V3D_SHOW_WORLD)
						ED_area_tag_redraw_regiontype(sa, RGN_TYPE_WINDOW);
					break;
			}
			break;
		case NC_MATERIAL:
			switch (wmn->data) {
				case ND_NODES:
					if (v3d->drawtype == OB_TEXTURE)
						ED_area_tag_redraw_regiontype(sa, RGN_TYPE_WINDOW);
					break;
			}
			break;
	}
}

const char *view3d_context_dir[] = {
	"selected_objects", "selected_bases", "selected_editable_objects",
	"selected_editable_bases", "visible_objects", "visible_bases", "selectable_objects", "selectable_bases",
	"active_base", "active_object", NULL
};

static int view3d_context(const bContext *C, const char *member, bContextDataResult *result)
{
	/* fallback to the scene layer, allows duplicate and other object operators to run outside the 3d view */

	if (CTX_data_dir(member)) {
		CTX_data_dir_set(result, view3d_context_dir);
	}
	else if (CTX_data_equals(member, "selected_objects") || CTX_data_equals(member, "selected_bases")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		Base *base;
		const bool selected_objects = CTX_data_equals(member, "selected_objects");

		for (base = scene->base.first; base; base = base->next) {
			if ((base->flag & SELECT) && (base->lay & lay)) {
				if ((base->object->restrictflag & OB_RESTRICT_VIEW) == 0) {
					if (selected_objects)
						CTX_data_id_list_add(result, &base->object->id);
					else
						CTX_data_list_add(result, &scene->id, &RNA_ObjectBase, base);
				}
			}
		}
		CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
		return 1;
	}
	else if (CTX_data_equals(member, "selected_editable_objects") || CTX_data_equals(member, "selected_editable_bases")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		Base *base;
		const bool selected_editable_objects = CTX_data_equals(member, "selected_editable_objects");

		for (base = scene->base.first; base; base = base->next) {
			if ((base->flag & SELECT) && (base->lay & lay)) {
				if ((base->object->restrictflag & OB_RESTRICT_VIEW) == 0) {
					if (0 == BKE_object_is_libdata(base->object)) {
						if (selected_editable_objects)
							CTX_data_id_list_add(result, &base->object->id);
						else
							CTX_data_list_add(result, &scene->id, &RNA_ObjectBase, base);
					}
				}
			}
		}
		CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
		return 1;
	}
	else if (CTX_data_equals(member, "visible_objects") || CTX_data_equals(member, "visible_bases")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		Base *base;
		const bool visible_objects = CTX_data_equals(member, "visible_objects");

		for (base = scene->base.first; base; base = base->next) {
			if (base->lay & lay) {
				if ((base->object->restrictflag & OB_RESTRICT_VIEW) == 0) {
					if (visible_objects)
						CTX_data_id_list_add(result, &base->object->id);
					else
						CTX_data_list_add(result, &scene->id, &RNA_ObjectBase, base);
				}
			}
		}
		CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
		return 1;
	}
	else if (CTX_data_equals(member, "selectable_objects") || CTX_data_equals(member, "selectable_bases")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		Base *base;
		const bool selectable_objects = CTX_data_equals(member, "selectable_objects");

		for (base = scene->base.first; base; base = base->next) {
			if (base->lay & lay) {
				if ((base->object->restrictflag & OB_RESTRICT_VIEW) == 0 && (base->object->restrictflag & OB_RESTRICT_SELECT) == 0) {
					if (selectable_objects)
						CTX_data_id_list_add(result, &base->object->id);
					else
						CTX_data_list_add(result, &scene->id, &RNA_ObjectBase, base);
				}
			}
		}
		CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
		return 1;
	}
	else if (CTX_data_equals(member, "active_base")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		if (scene->basact && (scene->basact->lay & lay)) {
			Object *ob = scene->basact->object;
			/* if hidden but in edit mode, we still display, can happen with animation */
			if ((ob->restrictflag & OB_RESTRICT_VIEW) == 0 || (ob->mode & OB_MODE_EDIT))
				CTX_data_pointer_set(result, &scene->id, &RNA_ObjectBase, scene->basact);
		}
		
		return 1;
	}
	else if (CTX_data_equals(member, "active_object")) {
		View3D *v3d = CTX_wm_view3d(C);
		Scene *scene = CTX_data_scene(C);
		const unsigned int lay = v3d ? v3d->lay : scene->lay;
		if (scene->basact && (scene->basact->lay & lay)) {
			Object *ob = scene->basact->object;
			if ((ob->restrictflag & OB_RESTRICT_VIEW) == 0 || (ob->mode & OB_MODE_EDIT))
				CTX_data_id_pointer_set(result, &scene->basact->object->id);
		}
		
		return 1;
	}
	else {
		return 0; /* not found */
	}

	return -1; /* found but not available */
}

static void view3d_id_remap(ScrArea *sa, SpaceLink *slink, ID *old_id, ID *new_id)
{
	View3D *v3d;
	ARegion *ar;
	bool is_local = false;

	if (!ELEM(GS(old_id->name), ID_OB, ID_MA, ID_IM, ID_MC)) {
		return;
	}

	for (v3d = (View3D *)slink; v3d; v3d = v3d->localvd, is_local = true) {
		if ((ID *)v3d->camera == old_id) {
			v3d->camera = (Object *)new_id;
			if (!new_id) {
				/* 3D view might be inactive, in that case needs to use slink->regionbase */
				ListBase *regionbase = (slink == sa->spacedata.first) ? &sa->regionbase : &slink->regionbase;
				for (ar = regionbase->first; ar; ar = ar->next) {
					if (ar->regiontype == RGN_TYPE_WINDOW) {
						RegionView3D *rv3d = is_local ? ((RegionView3D *)ar->regiondata)->localvd : ar->regiondata;
						if (rv3d && (rv3d->persp == RV3D_CAMOB)) {
							rv3d->persp = RV3D_PERSP;
						}
					}
				}
			}
		}
		if ((ID *)v3d->ob_centre == old_id) {
			v3d->ob_centre = (Object *)new_id;
			if (new_id == NULL) {  /* Otherwise, bonename may remain valid... We could be smart and check this, too? */
				v3d->ob_centre_bone[0] = '\0';
			}
		}

		if ((ID *)v3d->defmaterial == old_id) {
			v3d->defmaterial = (Material *)new_id;
		}
#if 0  /* XXX Deprecated? */
		if ((ID *)v3d->gpd == old_id) {
			v3d->gpd = (bGPData *)new_id;
		}
#endif

		if (ELEM(GS(old_id->name), ID_IM, ID_MC)) {
			for (BGpic *bgpic = v3d->bgpicbase.first; bgpic; bgpic = bgpic->next) {
				if ((ID *)bgpic->ima == old_id) {
					bgpic->ima = (Image *)new_id;
					id_us_min(old_id);
					id_us_plus(new_id);
				}
				if ((ID *)bgpic->clip == old_id) {
					bgpic->clip = (MovieClip *)new_id;
					id_us_min(old_id);
					id_us_plus(new_id);
				}
			}
		}

		if (is_local) {
			break;
		}
	}
}

/* only called once, from space/spacetypes.c */
void ED_spacetype_view3d(void)
{
	SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype view3d");
	ARegionType *art;
	
	st->spaceid = SPACE_VIEW3D;
	strncpy(st->name, "View3D", BKE_ST_MAXNAME);
	
	st->new = view3d_new;
	st->free = view3d_free;
	st->init = view3d_init;
	st->listener = space_view3d_listener;
	st->duplicate = view3d_duplicate;
	st->operatortypes = view3d_operatortypes;
	st->keymap = view3d_keymap;
	st->dropboxes = view3d_dropboxes;
	st->context = view3d_context;
	st->id_remap = view3d_id_remap;

	/* regions: main window */
	art = MEM_callocN(sizeof(ARegionType), "spacetype view3d main region");
	art->regionid = RGN_TYPE_WINDOW;
	art->keymapflag = ED_KEYMAP_GPENCIL;
	art->draw = view3d_main_region_draw;
	art->init = view3d_main_region_init;
	art->exit = view3d_main_region_exit;
	art->free = view3d_main_region_free;
	art->duplicate = view3d_main_region_duplicate;
	art->listener = view3d_main_region_listener;
	art->cursor = view3d_main_region_cursor;
	art->lock = 1;   /* can become flag, see BKE_spacedata_draw_locks */
	BLI_addhead(&st->regiontypes, art);
	
	/* regions: listview/buttons */
	art = MEM_callocN(sizeof(ARegionType), "spacetype view3d buttons region");
	art->regionid = RGN_TYPE_UI;
	art->prefsizex = 180; /* XXX */
	art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
	art->listener = view3d_buttons_region_listener;
	art->init = view3d_buttons_region_init;
	art->draw = view3d_buttons_region_draw;
	BLI_addhead(&st->regiontypes, art);

	view3d_buttons_register2(art);

	/* regions: tool(bar) */
	art = MEM_callocN(sizeof(ARegionType), "spacetype view3d tools region");
	art->regionid = RGN_TYPE_TOOLS;
	art->prefsizex = 160; /* XXX */
	art->prefsizey = 50; /* XXX */
	art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
	art->listener = view3d_buttons_region_listener;
	art->init = view3d_tools_region_init;
	art->draw = view3d_tools_region_draw;
	BLI_addhead(&st->regiontypes, art);
	

	/* regions: tool properties */
	art = MEM_callocN(sizeof(ARegionType), "spacetype view3d tool properties region");
	art->regionid = RGN_TYPE_TOOL_PROPS;
	art->prefsizex = 0;
	art->prefsizey = 120;
	art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
	art->listener = view3d_props_region_listener;
	art->init = view3d_tools_region_init;
	art->draw = view3d_tools_region_draw;
	BLI_addhead(&st->regiontypes, art);
	
	view3d_tool_props_register(art);
	
	
	/* regions: header */
	art = MEM_callocN(sizeof(ARegionType), "spacetype view3d header region");
	art->regionid = RGN_TYPE_HEADER;
	art->prefsizey = HEADERY;
	art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_FRAMES | ED_KEYMAP_HEADER;
	art->listener = view3d_header_region_listener;
	art->init = view3d_header_region_init;
	art->draw = view3d_header_region_draw;

#if 0
	// similar to view3d_tools_region_draw from above.
	// view3d_header_region_draw -> ED_region_header
    //which calls which iterates thru all header types defining the ui...same for tools.
	void ED_region_header(const bContext *C, ARegion *ar)
	{
		uiStyle *style = UI_style_get_dpi();
		uiBlock *block;
		uiLayout *layout;
		HeaderType *ht;
		Header header = {NULL};
		int maxco, xco, yco;
		int headery = ED_area_headersize();

		/* clear */
		UI_ThemeClearColor((ED_screen_area_active(C)) ? TH_HEADER : TH_HEADERDESEL);
		glClear(GL_COLOR_BUFFER_BIT);

		/* set view2d view matrix for scrolling (without scrollers) */
		UI_view2d_view_ortho(&ar->v2d);

		xco = maxco = 0.4f * UI_UNIT_X;
		yco = headery - floor(0.2f * UI_UNIT_Y);

		/* draw all headers types */
		for (ht = ar->type->headertypes.first; ht; ht = ht->next) {
			block = UI_block_begin(C, ar, ht->idname, UI_EMBOSS);
			layout = UI_block_layout(block, UI_LAYOUT_HORIZONTAL, UI_LAYOUT_HEADER, xco, yco, UI_UNIT_Y, 1, 0, style);

			if (ht->draw) {
				header.type = ht;
				header.layout = layout;
				ht->draw(C, &header);

				/* for view2d */
				xco = uiLayoutGetWidth(layout);
				if (xco > maxco)
					maxco = xco;
			}

			UI_block_layout_resolve(block, &xco, &yco);

			/* for view2d */
			if (xco > maxco)
				maxco = xco;

			UI_block_end(C, block);
			UI_block_draw(C, block);
		}

		/* always as last  */
		UI_view2d_totRect_set(&ar->v2d, maxco + UI_UNIT_X + 80, headery);

		/* restore view matrix? */
		UI_view2d_view_restore(C);
	}
#endif


	header_register(art);

	BLI_addhead(&st->regiontypes, art);
	
	BKE_spacetype_register(st);
}

/* ******************* view3d space & buttons ************** */
#define B_REDR              2
#define B_OBJECTPANELMEDIAN 1008

#define NBR_TRANSFORM_PROPERTIES 8
/* Default, void context.
 * WARNING! The "" context is not the same as no (NULL) context at mo/boost::locale level!
 * NOTE: We translate BLT_I18NCONTEXT_DEFAULT as BLT_I18NCONTEXT_DEFAULT_BPY in Python, as we can't use "natural"
 *       None value in rna string properties... :/
 *       The void string "" is also interpreted as BLT_I18NCONTEXT_DEFAULT.
 *       For perf reason, we only use the first char to detect this context, so other contexts should never start
 *       with the same char!
 */
#define BLT_I18NCONTEXT_DEFAULT NULL
#define BLT_I18NCONTEXT_DEFAULT_BPYRNA "*"

/* Default context for operator names/labels. */
#define BLT_I18NCONTEXT_OPERATOR_DEFAULT "Operator"

/* Context for events/keymaps (necessary, since those often use one or two letters,
 * easy to get collisions with other areas...). */
#define BLT_I18NCONTEXT_UI_EVENTS "UI_Events_KeyMaps"

/* Mark the msgid applies to several elements (needed in some cases, as english adjectives have no plural mark :( ). */
#define BLT_I18NCONTEXT_PLURAL "Plural"

/* ID-types contexts. */
/* WARNING! Keep it in sync with idtypes in blenkernel/intern/idcode.c */
#define BLT_I18NCONTEXT_ID_ACTION               "Action"
#define BLT_I18NCONTEXT_ID_ARMATURE             "Armature"
#define BLT_I18NCONTEXT_ID_BRUSH                "Brush"
#define BLT_I18NCONTEXT_ID_CAMERA               "Camera"
#define BLT_I18NCONTEXT_ID_CACHEFILE            "CacheFile"
#define BLT_I18NCONTEXT_ID_CURVE                "Curve"
#define BLT_I18NCONTEXT_ID_FREESTYLELINESTYLE   "FreestyleLineStyle"
#define BLT_I18NCONTEXT_ID_GPENCIL              "GPencil"
#define BLT_I18NCONTEXT_ID_GROUP                "Group"
#define BLT_I18NCONTEXT_ID_ID                   "ID"
#define BLT_I18NCONTEXT_ID_IMAGE                "Image"
/*#define BLT_I18NCONTEXT_ID_IPO                  "Ipo"*/ /* Deprecated */
#define BLT_I18NCONTEXT_ID_SHAPEKEY             "Key"
#define BLT_I18NCONTEXT_ID_LAMP                 "Lamp"
#define BLT_I18NCONTEXT_ID_LIBRARY              "Library"
#define BLT_I18NCONTEXT_ID_LATTICE              "Lattice"
#define BLT_I18NCONTEXT_ID_MATERIAL             "Material"
#define BLT_I18NCONTEXT_ID_METABALL             "Metaball"
#define BLT_I18NCONTEXT_ID_MESH                 "Mesh"
#define BLT_I18NCONTEXT_ID_NODETREE             "NodeTree"
#define BLT_I18NCONTEXT_ID_OBJECT               "Object"
#define BLT_I18NCONTEXT_ID_PAINTCURVE           "PaintCurve"
#define BLT_I18NCONTEXT_ID_PALETTE              "Palette"
#define BLT_I18NCONTEXT_ID_PARTICLESETTINGS     "ParticleSettings"
#define BLT_I18NCONTEXT_ID_SCENE                "Scene"
#define BLT_I18NCONTEXT_ID_SCREEN               "Screen"
#define BLT_I18NCONTEXT_ID_SEQUENCE             "Sequence"
#define BLT_I18NCONTEXT_ID_SPEAKER              "Speaker"
#define BLT_I18NCONTEXT_ID_SOUND                "Sound"
#define BLT_I18NCONTEXT_ID_TEXTURE              "Texture"
#define BLT_I18NCONTEXT_ID_TEXT                 "Text"
#define BLT_I18NCONTEXT_ID_VFONT                "VFont"
#define BLT_I18NCONTEXT_ID_WORLD                "World"
#define BLT_I18NCONTEXT_ID_WINDOWMANAGER        "WindowManager"
#define BLT_I18NCONTEXT_ID_MOVIECLIP            "MovieClip"
#define BLT_I18NCONTEXT_ID_MASK                 "Mask"


/* temporary struct for storing transform properties */
typedef struct {
	float ob_eul[4];   /* used for quat too... */
	float ob_scale[3]; /* need temp space due to linked values */
	float ob_dims[3];
	short link_scale;
	float ve_median[NBR_TRANSFORM_PROPERTIES];
} TransformProperties;

/* EXPERIMENTAL... let's try to create the header layout here instead of python via
 * RNA / Python scripts
 */

// static StructRNA *rna_Header_register(Main *bmain, ReportList *reports, void *data, const char *identifier,
// taken from rna_ui.c

static void header_register(ARegionType* art)
{
#if 0
//	ARegionType *art;
	HeaderType *ht = {NULL};//, dummyht = {NULL};
//	Header dummyheader = {NULL};
//	PointerRNA dummyhtr;
//	int have_function[1];

	/* setup dummy header & header type to store static properties in */
//	dummyheader.type->ext = NULL; //&dummyht;
//	dummyheader.type->id = "doesn't matter";
//	dummyheader.type->space_type = SPACE_VIEW3D;
//	dummyheader.type->next = NULL; // this is probably for different modes as it changes the ui layout buttons, etc.
//	dummyheader.type->draw = NULL; // null for now :-(
//	RNA_pointer_create(NULL, &RNA_Header, &dummyheader, &dummyhtr);


#if 0
	// looks like most of this stuff is rna / python registration, we do manually so not needed!
	// validate the python class */
	if (validate(&dummyhtr, data, have_function) != 0)
		return NULL;

	if (strlen(identifier) >= sizeof(dummyht.idname)) {
		BKE_reportf(reports, RPT_ERROR, "Registering header class: '%s' is too long, maximum length is %d",
		            identifier, (int)sizeof(dummyht.idname));
		return NULL;
	}

	if (!(art = region_type_find(reports, dummyht.space_type, RGN_TYPE_HEADER)))
		return NULL;

	/* check if we have registered this header type before, and remove it */
	for (ht = art->headertypes.first; ht; ht = ht->next) {
		if (STREQ(ht->idname, dummyht.idname)) {
			if (ht->ext.srna)
				rna_Header_unregister(bmain, ht->ext.srna);
			break;
		}
	}
#endif

	/* create a new header type */
	ht = MEM_callocN(sizeof(HeaderType), "python buttons header");
//	memcpy(ht, &dummyht, sizeof(dummyht));

//	ht->ext = NULL; //&dummyht;
//	ht->idname = '';
	ht->space_type = SPACE_VIEW3D;
	ht->next = NULL; // this is probably for different modes as it changes the ui layout buttons, etc.
	ht->draw = NULL; // null for now :-(

//	ht->ext.srna = RNA_def_struct_ptr(&BLENDER_RNA, ht->idname, &RNA_Header);
//	ht->ext.data = data;
//	ht->ext.call = call;
//	ht->ext.free = free;
//	RNA_struct_blender_type_set(ht->ext.srna, ht);

//	ht->draw = (have_function[0]) ? header_draw : NULL;

	BLI_addtail(&art->headertypes, ht);

	/* update while blender is running */
	WM_main_add_notifier(NC_WINDOW, NULL);

//	return ht->ext.srna;
#endif

	HeaderType *ht;

	ht = MEM_callocN(sizeof(PanelType), "spacetype view3d panel object");
	strcpy(ht->idname, "VIEW3D_HT_header");
//	strcpy(ht->label, "HeaderBRAD");  /* XXX C panels not  available through RNA (bpy.types)! */
//	strcpy(ht->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
	ht->draw = view3d_header;
//	ht->poll = view3d_header_poll;
	BLI_addtail(&art->headertypes, ht);

}

static void view3d_header(const bContext *C, Header *he)
{
	uiBlock *block;
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	Object *ob = scene->basact->object;
	uiLayout *lo, *col;

	block = uiLayoutGetBlock(he->layout);
//	UI_block_func_handle_set(block, do_view3d_region_header, NULL);

	col = uiLayoutColumn(he->layout, false);
	lo = uiLayoutRow(he->layout, false);

	uiTemplateHeader3D(lo, C);

#if 0
	PointerRNA obptr;

	RNA_id_pointer_create(&ob->id, &obptr);
	v3d_header_butsR(col, &obptr);
#endif
}

static void v3d_header_butsR(uiLayout *layout, PointerRNA *ptr)
{
	uiLayout *row;

	row = uiLayoutRow(layout, true);

//	colsub = uiLayoutColumn(split, true);
//	uiItemR(colsub, ptr, "location", 0, NULL, ICON_NONE);
//	colsub = uiLayoutColumn(split, true);

	uiItemL(row, "this is the label", ICON_NONE);
	uiItemL(row, "this is the label2", ICON_NONE);
//	uiItemR(colsub, ptr, "lock_location", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

//	split = uiLayoutSplit(layout, 0.8f, false);


#if 0
	switch (RNA_enum_get(ptr, "rotation_mode")) {
		case ROT_MODE_QUAT: /* quaternion */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_quaternion", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "lock_rotations_4d", UI_ITEM_R_TOGGLE, "4L", ICON_NONE);
			if (RNA_boolean_get(ptr, "lock_rotations_4d"))
				uiItemR(colsub, ptr, "lock_rotation_w", UI_ITEM_R_TOGGLE + UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			else
				uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
		case ROT_MODE_AXISANGLE: /* axis angle */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_axis_angle", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "lock_rotations_4d", UI_ITEM_R_TOGGLE, "4L", ICON_NONE);
			if (RNA_boolean_get(ptr, "lock_rotations_4d"))
				uiItemR(colsub, ptr, "lock_rotation_w", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			else
				uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
		default: /* euler rotations */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_euler", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
	}
	uiItemR(layout, ptr, "rotation_mode", 0, "", ICON_NONE);

	split = uiLayoutSplit(layout, 0.8f, false);
	colsub = uiLayoutColumn(split, true);
	uiItemR(colsub, ptr, "scale", 0, NULL, ICON_NONE);
	colsub = uiLayoutColumn(split, true);
	uiItemL(colsub, "", ICON_NONE);
	uiItemR(colsub, ptr, "lock_scale", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

	if (ptr->type == &RNA_Object) {
		Object *ob = ptr->data;
		/* dimensions and editmode just happen to be the same checks */
		if (OB_TYPE_SUPPORT_EDITMODE(ob->type)) {
			uiItemR(layout, ptr, "dimensions", 0, NULL, ICON_NONE);
		}
	}
#endif

	printf("calling draw routine for view3e header\n");
}




/////////////////////////////// PANEL DRAW for TOOLS ///////////////////////////////////////////


static void v3d_transform_butsR(uiLayout *layout, PointerRNA *ptr)
{
	uiLayout *split, *colsub;

	split = uiLayoutSplit(layout, 0.8f, false);

	colsub = uiLayoutColumn(split, true);
	uiItemR(colsub, ptr, "location", 0, NULL, ICON_NONE);
	colsub = uiLayoutColumn(split, true);
	uiItemL(colsub, "", ICON_NONE);
	uiItemR(colsub, ptr, "lock_location", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

	split = uiLayoutSplit(layout, 0.8f, false);

	switch (RNA_enum_get(ptr, "rotation_mode")) {
		case ROT_MODE_QUAT: /* quaternion */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_quaternion", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "lock_rotations_4d", UI_ITEM_R_TOGGLE, "4L", ICON_NONE);
			if (RNA_boolean_get(ptr, "lock_rotations_4d"))
				uiItemR(colsub, ptr, "lock_rotation_w", UI_ITEM_R_TOGGLE + UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			else
				uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
		case ROT_MODE_AXISANGLE: /* axis angle */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_axis_angle", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "lock_rotations_4d", UI_ITEM_R_TOGGLE, "4L", ICON_NONE);
			if (RNA_boolean_get(ptr, "lock_rotations_4d"))
				uiItemR(colsub, ptr, "lock_rotation_w", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			else
				uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
		default: /* euler rotations */
			colsub = uiLayoutColumn(split, true);
			uiItemR(colsub, ptr, "rotation_euler", 0, "RotationBRAD", ICON_NONE);
			colsub = uiLayoutColumn(split, true);
			uiItemL(colsub, "", ICON_NONE);
			uiItemR(colsub, ptr, "lock_rotation", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);
			break;
	}
	uiItemR(layout, ptr, "rotation_mode", 0, "", ICON_NONE);

	split = uiLayoutSplit(layout, 0.8f, false);
	colsub = uiLayoutColumn(split, true);
	uiItemR(colsub, ptr, "scale", 0, NULL, ICON_NONE);
	colsub = uiLayoutColumn(split, true);
	uiItemL(colsub, "", ICON_NONE);
	uiItemR(colsub, ptr, "lock_scale", UI_ITEM_R_TOGGLE | UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

	if (ptr->type == &RNA_Object) {
		Object *ob = ptr->data;
		/* dimensions and editmode just happen to be the same checks */
		if (OB_TYPE_SUPPORT_EDITMODE(ob->type)) {
			uiItemR(layout, ptr, "dimensions", 0, NULL, ICON_NONE);
		}
	}
}

static void do_view3d_region_buttons(bContext *C, void *UNUSED(index), int event)
{
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = CTX_wm_view3d(C);
	Object *ob = OBACT;

	switch (event) {

		case B_REDR:
			ED_area_tag_redraw(CTX_wm_area(C));
			return; /* no notifier! */

		case B_OBJECTPANELMEDIAN:
			if (ob) {
//				v3d_editvertex_buts(NULL, v3d, ob, 1.0);
//				DAG_id_tag_update(&ob->id, OB_RECALC_DATA);
			}
			break;
	}

	/* default for now */
	WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, v3d);
}

static int view3d_panel_transform_poll(const bContext *C, PanelType *UNUSED(pt))
{
	Scene *scene = CTX_data_scene(C);
	return (scene->basact != NULL);
}

static void view3d_panel_transform(const bContext *C, Panel *pa)
{
	uiBlock *block;
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	Object *ob = scene->basact->object;
	uiLayout *col;

	block = uiLayoutGetBlock(pa->layout);
	UI_block_func_handle_set(block, do_view3d_region_buttons, NULL);

	col = uiLayoutColumn(pa->layout, false);

//	if (ob == obedit) {
#if 0
		if (ob->type == OB_ARMATURE) {
			v3d_editarmature_buts(col, ob);
		}
		else if (ob->type == OB_MBALL) {
			v3d_editmetaball_buts(col, ob);
		}
#endif
//		else {
/* we evenutally want to add this back in...
			View3D *v3d = CTX_wm_view3d(C);
			const float lim = 10000.0f * max_ff(1.0f, ED_view3d_grid_scale(scene, v3d, NULL));
			v3d_editvertex_buts(col, v3d, ob, lim);
*/
//		}
//	}
#if 0
	else if (ob->mode & OB_MODE_POSE) {
		v3d_posearmature_buts(col, ob);
	}
#endif
//	else {
		PointerRNA obptr;

		RNA_id_pointer_create(&ob->id, &obptr);
		v3d_transform_butsR(col, &obptr);
//	}
}

static void view3d_buttons_register2(ARegionType *art)
{
	PanelType *pt;

	pt = MEM_callocN(sizeof(PanelType), "spacetype view3d panel object");
	strcpy(pt->idname, "VIEW3D_PT_transform");
	strcpy(pt->label, "TransformBRAD");  /* XXX C panels not  available through RNA (bpy.types)! */
	strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
	pt->draw = view3d_panel_transform;
	pt->poll = view3d_panel_transform_poll;
	BLI_addtail(&art->paneltypes, pt);

#if 0
	// again, we want this back for edit mode...
	pt = MEM_callocN(sizeof(PanelType), "spacetype view3d panel vgroup");
	strcpy(pt->idname, "VIEW3D_PT_vgroup");
	strcpy(pt->label, "Vertex Weights");  /* XXX C panels are not available through RNA (bpy.types)! */
	strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
	pt->draw = view3d_panel_vgroup;
	pt->poll = view3d_panel_vgroup_poll;
	BLI_addtail(&art->paneltypes, pt);
#endif
}

#if 0
static void view3d_panel_vgroup(const bContext *C, Panel *pa)
{
	uiBlock *block = uiLayoutAbsoluteBlock(pa->layout);
	Scene *scene = CTX_data_scene(C);
	Object *ob = scene->basact->object;

	MDeformVert *dv;

	dv = ED_mesh_active_dvert_get_only(ob);

	if (dv && dv->totweight) {
		ToolSettings *ts = scene->toolsettings;

		wmOperatorType *ot;
		PointerRNA op_ptr, tools_ptr;
		PointerRNA *but_ptr;

		uiLayout *col, *bcol;
		uiLayout *row;
		uiBut *but;
		bDeformGroup *dg;
		unsigned int i;
		int subset_count, vgroup_tot;
		const bool *vgroup_validmap;
		eVGroupSelect subset_type = ts->vgroupsubset;
		int yco = 0;
		int lock_count = 0;

		UI_block_func_handle_set(block, do_view3d_vgroup_buttons, NULL);

		bcol = uiLayoutColumn(pa->layout, true);
		row = uiLayoutRow(bcol, true); /* The filter button row */

		RNA_pointer_create(NULL, &RNA_ToolSettings, ts, &tools_ptr);
		uiItemR(row, &tools_ptr, "vertex_group_subset", UI_ITEM_R_EXPAND, NULL, ICON_NONE);

		col = uiLayoutColumn(bcol, true);

		vgroup_validmap = BKE_object_defgroup_subset_from_select_type(ob, subset_type, &vgroup_tot, &subset_count);
		for (i = 0, dg = ob->defbase.first; dg; i++, dg = dg->next) {
			bool locked = (dg->flag & DG_LOCK_WEIGHT) != 0;
			if (vgroup_validmap[i]) {
				MDeformWeight *dw = defvert_find_index(dv, i);
				if (dw) {
					int x, xco = 0;
					int icon;
					uiLayout *split = uiLayoutSplit(col, 0.45, true);
					row = uiLayoutRow(split, true);

					/* The Weight Group Name */

					ot = WM_operatortype_find("OBJECT_OT_vertex_weight_set_active", true);
					but = uiDefButO_ptr(block, UI_BTYPE_BUT, ot, WM_OP_EXEC_DEFAULT, dg->name,
					                    xco, yco, (x = UI_UNIT_X * 5), UI_UNIT_Y, "");
					but_ptr = UI_but_operator_ptr_get(but);
					RNA_int_set(but_ptr, "weight_group", i);
					UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
					if (ob->actdef != i + 1) {
						UI_but_flag_enable(but, UI_BUT_INACTIVE);
					}
					xco += x;

					row = uiLayoutRow(split, true);
					uiLayoutSetEnabled(row, !locked);

					/* The weight group value */
					/* To be reworked still */
					but = uiDefButF(block, UI_BTYPE_NUM, B_VGRP_PNL_EDIT_SINGLE + i, "",
					                xco, yco, (x = UI_UNIT_X * 4), UI_UNIT_Y,
					                &dw->weight, 0.0, 1.0, 1, 3, "");
					UI_but_drawflag_enable(but, UI_BUT_TEXT_LEFT);
					if (locked) {
						lock_count++;
					}
					xco += x;

					/* The weight group paste function */
					icon = (locked) ? ICON_BLANK1 : ICON_PASTEDOWN;
					op_ptr = uiItemFullO(row, "OBJECT_OT_vertex_weight_paste", "", icon, NULL, WM_OP_INVOKE_DEFAULT, UI_ITEM_O_RETURN_PROPS);
					RNA_int_set(&op_ptr, "weight_group", i);

					/* The weight entry delete function */
					icon = (locked) ? ICON_LOCKED : ICON_X;
					op_ptr = uiItemFullO(row, "OBJECT_OT_vertex_weight_delete", "", icon, NULL, WM_OP_INVOKE_DEFAULT, UI_ITEM_O_RETURN_PROPS);
					RNA_int_set(&op_ptr, "weight_group", i);

					yco -= UI_UNIT_Y;
				}
			}
		}
		MEM_freeN((void *)vgroup_validmap);

		yco -= 2;

		col = uiLayoutColumn(pa->layout, true);
		row = uiLayoutRow(col, true);

		ot = WM_operatortype_find("OBJECT_OT_vertex_weight_normalize_active_vertex", 1);
		but = uiDefButO_ptr(block, UI_BTYPE_BUT, ot, WM_OP_EXEC_DEFAULT, "Normalize",
		                    0, yco, UI_UNIT_X * 5, UI_UNIT_Y,
		                    TIP_("Normalize weights of active vertex (if affected groups are unlocked)"));
		if (lock_count) {
			UI_but_flag_enable(but, UI_BUT_DISABLED);
		}

		ot = WM_operatortype_find("OBJECT_OT_vertex_weight_copy", 1);
		but = uiDefButO_ptr(block, UI_BTYPE_BUT, ot, WM_OP_EXEC_DEFAULT, "Copy",
		                    UI_UNIT_X * 5, yco, UI_UNIT_X * 5, UI_UNIT_Y,
		                    TIP_("Copy active vertex to other selected vertices (if affected groups are unlocked)"));
		if (lock_count) {
			UI_but_flag_enable(but, UI_BUT_DISABLED);
		}

	}
}
#endif

#if 0
static int view3d_panel_vgroup_poll(const bContext *C, PanelType *UNUSED(pt))
{
	Scene *scene = CTX_data_scene(C);
	Object *ob = OBACT;
	if (ob && (BKE_object_is_in_editmode_vgroup(ob) ||
	           BKE_object_is_in_wpaint_select_vert(ob)))
	{
		MDeformVert *dvert_act = ED_mesh_active_dvert_get_only(ob);
		if (dvert_act) {
			return (dvert_act->totweight != 0);
		}
	}

	return false;
}
#endif

#if 0

#define IFACE_(str) str

/* is used for both read and write... */
static void v3d_editvertex_buts(uiLayout *layout, View3D *v3d, Object *ob, float lim)
{
/* Get rid of those ugly magic numbers, even in a single func they become confusing! */
/* Location, common to all. */
/* Next three *must* remain contiguous (used as array)! */
#define LOC_X        0
#define LOC_Y        1
#define LOC_Z        2
/* Meshes... */
#define M_BV_WEIGHT  3
/* Next two *must* remain contiguous (used as array)! */
#define M_SKIN_X     4
#define M_SKIN_Y     5
#define M_BE_WEIGHT  6
#define M_CREASE     7
/* Curves... */
#define C_BWEIGHT    3
#define C_WEIGHT     4
#define C_RADIUS     5
#define C_TILT       6
/*Lattice... */
#define L_WEIGHT     4

	uiBlock *block = (layout) ? uiLayoutAbsoluteBlock(layout) : NULL;
	TransformProperties *tfp;
	float median[NBR_TRANSFORM_PROPERTIES], ve_median[NBR_TRANSFORM_PROPERTIES];
	int tot, totedgedata, totcurvedata, totlattdata, totcurvebweight;
	bool has_meshdata = false;
	bool has_skinradius = false;
	PointerRNA data_ptr;

	copy_vn_fl(median, NBR_TRANSFORM_PROPERTIES, 0.0f);
	tot = totedgedata = totcurvedata = totlattdata = totcurvebweight = 0;

	/* make sure we got storage */
	if (v3d->properties_storage == NULL)
		v3d->properties_storage = MEM_callocN(sizeof(TransformProperties), "TransformProperties");
	tfp = v3d->properties_storage;

	if (ob->type == OB_MESH) {
		Mesh *me = ob->data;
		BMEditMesh *em = me->edit_btmesh;
		BMesh *bm = em->bm;
		BMVert *eve;
		BMEdge *eed;
		BMIter iter;

		const int cd_vert_bweight_offset = CustomData_get_offset(&bm->vdata, CD_BWEIGHT);
		const int cd_vert_skin_offset    = CustomData_get_offset(&bm->vdata, CD_MVERT_SKIN);
		const int cd_edge_bweight_offset = CustomData_get_offset(&bm->edata, CD_BWEIGHT);
		const int cd_edge_crease_offset  = CustomData_get_offset(&bm->edata, CD_CREASE);

		has_skinradius = (cd_vert_skin_offset != -1);

		if (bm->totvertsel) {
			BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
				if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
					tot++;
					add_v3_v3(&median[LOC_X], eve->co);

					if (cd_vert_bweight_offset != -1) {
						median[M_BV_WEIGHT] += BM_ELEM_CD_GET_FLOAT(eve, cd_vert_bweight_offset);
					}

					if (has_skinradius) {
						MVertSkin *vs = BM_ELEM_CD_GET_VOID_P(eve, cd_vert_skin_offset);
						add_v2_v2(&median[M_SKIN_X], vs->radius); /* Third val not used currently. */
					}
				}
			}
		}

		if ((cd_edge_bweight_offset != -1) || (cd_edge_crease_offset  != -1)) {
			if (bm->totedgesel) {
				BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
					if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
						if (cd_edge_bweight_offset != -1) {
							median[M_BE_WEIGHT] += BM_ELEM_CD_GET_FLOAT(eed, cd_edge_bweight_offset);
						}

						if (cd_edge_crease_offset != -1) {
							median[M_CREASE] += BM_ELEM_CD_GET_FLOAT(eed, cd_edge_crease_offset);
						}

						totedgedata++;
					}
				}
			}
		}
		else {
			totedgedata = bm->totedgesel;
		}

		has_meshdata = (tot || totedgedata);
	}
	else if (ob->type == OB_CURVE || ob->type == OB_SURF) {
		Curve *cu = ob->data;
		Nurb *nu;
		BPoint *bp;
		BezTriple *bezt;
		int a;
		ListBase *nurbs = BKE_curve_editNurbs_get(cu);
		StructRNA *seltype = NULL;
		void *selp = NULL;

		nu = nurbs->first;
		while (nu) {
			if (nu->type == CU_BEZIER) {
				bezt = nu->bezt;
				a = nu->pntsu;
				while (a--) {
					if (bezt->f2 & SELECT) {
						add_v3_v3(&median[LOC_X], bezt->vec[1]);
						tot++;
						median[C_WEIGHT] += bezt->weight;
						median[C_RADIUS] += bezt->radius;
						median[C_TILT] += bezt->alfa;
						if (!totcurvedata) { /* I.e. first time... */
							selp = bezt;
							seltype = &RNA_BezierSplinePoint;
						}
						totcurvedata++;
					}
					else {
						if (bezt->f1 & SELECT) {
							add_v3_v3(&median[LOC_X], bezt->vec[0]);
							tot++;
						}
						if (bezt->f3 & SELECT) {
							add_v3_v3(&median[LOC_X], bezt->vec[2]);
							tot++;
						}
					}
					bezt++;
				}
			}
			else {
				bp = nu->bp;
				a = nu->pntsu * nu->pntsv;
				while (a--) {
					if (bp->f1 & SELECT) {
						add_v3_v3(&median[LOC_X], bp->vec);
						median[C_BWEIGHT] += bp->vec[3];
						totcurvebweight++;
						tot++;
						median[C_WEIGHT] += bp->weight;
						median[C_RADIUS] += bp->radius;
						median[C_TILT] += bp->alfa;
						if (!totcurvedata) { /* I.e. first time... */
							selp = bp;
							seltype = &RNA_SplinePoint;
						}
						totcurvedata++;
					}
					bp++;
				}
			}
			nu = nu->next;
		}

		if (totcurvedata == 1)
			RNA_pointer_create(&cu->id, seltype, selp, &data_ptr);
	}
	else if (ob->type == OB_LATTICE) {
		Lattice *lt = ob->data;
		BPoint *bp;
		int a;
		StructRNA *seltype = NULL;
		void *selp = NULL;

		a = lt->editlatt->latt->pntsu * lt->editlatt->latt->pntsv * lt->editlatt->latt->pntsw;
		bp = lt->editlatt->latt->def;
		while (a--) {
			if (bp->f1 & SELECT) {
				add_v3_v3(&median[LOC_X], bp->vec);
				tot++;
				median[L_WEIGHT] += bp->weight;
				if (!totlattdata) { /* I.e. first time... */
					selp = bp;
					seltype = &RNA_LatticePoint;
				}
				totlattdata++;
			}
			bp++;
		}

		if (totlattdata == 1)
			RNA_pointer_create(&lt->id, seltype, selp, &data_ptr);
	}

	if (tot == 0) {
		uiDefBut(block, UI_BTYPE_LABEL, 0, IFACE_("Nothing selected"), 0, 130, 200, 20, NULL, 0, 0, 0, 0, "");
		return;
	}

	/* Location, X/Y/Z */
	mul_v3_fl(&median[LOC_X], 1.0f / (float)tot);
	if (v3d->flag & V3D_GLOBAL_STATS)
		mul_m4_v3(ob->obmat, &median[LOC_X]);

	if (has_meshdata) {
		if (totedgedata) {
			median[M_CREASE] /= (float)totedgedata;
			median[M_BE_WEIGHT] /= (float)totedgedata;
		}
		if (tot) {
			median[M_BV_WEIGHT] /= (float)tot;
			if (has_skinradius) {
				median[M_SKIN_X] /= (float)tot;
				median[M_SKIN_Y] /= (float)tot;
			}
		}
	}
	else if (totcurvedata) {
		if (totcurvebweight) {
			median[C_BWEIGHT] /= (float)totcurvebweight;
		}
		median[C_WEIGHT] /= (float)totcurvedata;
		median[C_RADIUS] /= (float)totcurvedata;
		median[C_TILT] /= (float)totcurvedata;
	}
	else if (totlattdata) {
		median[L_WEIGHT] /= (float)totlattdata;
	}

	if (block) { /* buttons */
		uiBut *but;
		int yi = 200;
		const float tilt_limit = DEG2RADF(21600.0f);
		const int buth = 20 * UI_DPI_FAC;
		const int but_margin = 2;
		const char *c;

		memcpy(tfp->ve_median, median, sizeof(tfp->ve_median));

		UI_block_align_begin(block);
		if (tot == 1) {
			if (totcurvedata) /* Curve */
				c = IFACE_("Control Point:");
			else /* Mesh or lattice */
				c = IFACE_("Vertex:");
		}
		else
			c = IFACE_("Median:");
		uiDefBut(block, UI_BTYPE_LABEL, 0, c, 0, yi -= buth, 200, buth, NULL, 0, 0, 0, 0, "");

		UI_block_align_begin(block);

		/* Should be no need to translate these. */
		but = uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("X:"), 0, yi -= buth, 200, buth,
		                &(tfp->ve_median[LOC_X]), -lim, lim, 10, RNA_TRANSLATION_PREC_DEFAULT, "");
		UI_but_unit_type_set(but, PROP_UNIT_LENGTH);
		but = uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Y:"), 0, yi -= buth, 200, buth,
		                &(tfp->ve_median[LOC_Y]), -lim, lim, 10, RNA_TRANSLATION_PREC_DEFAULT, "");
		UI_but_unit_type_set(but, PROP_UNIT_LENGTH);
		but = uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Z:"), 0, yi -= buth, 200, buth,
		                &(tfp->ve_median[LOC_Z]), -lim, lim, 10, RNA_TRANSLATION_PREC_DEFAULT, "");
		UI_but_unit_type_set(but, PROP_UNIT_LENGTH);

		if (totcurvebweight == tot) {
			uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("W:"), 0, yi -= buth, 200, buth,
			          &(tfp->ve_median[C_BWEIGHT]), 0.01, 100.0, 1, 3, "");
		}

		UI_block_align_begin(block);
		uiDefButBitS(block, UI_BTYPE_TOGGLE, V3D_GLOBAL_STATS, B_REDR, IFACE_("Global"),
		             0, yi -= buth + but_margin, 100, buth,
		             &v3d->flag, 0, 0, 0, 0, TIP_("Displays global values"));
		uiDefButBitS(block, UI_BTYPE_TOGGLE_N, V3D_GLOBAL_STATS, B_REDR, IFACE_("Local"),
		             100, yi, 100, buth,
		             &v3d->flag, 0, 0, 0, 0, TIP_("Displays local values"));
		UI_block_align_end(block);

		/* Meshes... */
		if (has_meshdata) {
			if (tot) {
				uiDefBut(block, UI_BTYPE_LABEL, 0, tot == 1 ? IFACE_("Vertex Data:") : IFACE_("Vertices Data:"),
				         0, yi -= buth + but_margin, 200, buth, NULL, 0.0, 0.0, 0, 0, "");
				/* customdata layer added on demand */
				uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN,
				          tot == 1 ? IFACE_("Bevel Weight:") : IFACE_("Mean Bevel Weight:"),
				          0, yi -= buth + but_margin, 200, buth,
				          &(tfp->ve_median[M_BV_WEIGHT]), 0.0, 1.0, 1, 2, TIP_("Vertex weight used by Bevel modifier"));
			}
			if (has_skinradius) {
				UI_block_align_begin(block);
				uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN,
				          tot == 1 ? IFACE_("Radius X:") : IFACE_("Mean Radius X:"),
				          0, yi -= buth + but_margin, 200, buth,
				          &(tfp->ve_median[M_SKIN_X]), 0.0, 100.0, 1, 3, TIP_("X radius used by Skin modifier"));
				uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN,
				          tot == 1 ? IFACE_("Radius Y:") : IFACE_("Mean Radius Y:"),
				          0, yi -= buth + but_margin, 200, buth,
				          &(tfp->ve_median[M_SKIN_Y]), 0.0, 100.0, 1, 3, TIP_("Y radius used by Skin modifier"));
				UI_block_align_end(block);
			}
			if (totedgedata) {
				uiDefBut(block, UI_BTYPE_LABEL, 0, totedgedata == 1 ? IFACE_("Edge Data:") : IFACE_("Edges Data:"),
				         0, yi -= buth + but_margin, 200, buth, NULL, 0.0, 0.0, 0, 0, "");
				/* customdata layer added on demand */
				uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN,
				          totedgedata == 1 ? IFACE_("Bevel Weight:") : IFACE_("Mean Bevel Weight:"),
				          0, yi -= buth + but_margin, 200, buth,
				          &(tfp->ve_median[M_BE_WEIGHT]), 0.0, 1.0, 1, 2, TIP_("Edge weight used by Bevel modifier"));
				/* customdata layer added on demand */
				uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN,
				          totedgedata == 1 ? IFACE_("Crease:") : IFACE_("Mean Crease:"),
				          0, yi -= buth + but_margin, 200, buth,
				          &(tfp->ve_median[M_CREASE]), 0.0, 1.0, 1, 2, TIP_("Weight used by the Subdivision Surface modifier"));
			}
		}
		/* Curve... */
		else if (totcurvedata == 1) {
			uiDefButR(block, UI_BTYPE_NUM, 0, IFACE_("Weight:"), 0, yi -= buth + but_margin, 200, buth,
			          &data_ptr, "weight_softbody", 0, 0.0, 1.0, 1, 3, NULL);
			uiDefButR(block, UI_BTYPE_NUM, 0, IFACE_("Radius:"), 0, yi -= buth + but_margin, 200, buth,
			          &data_ptr, "radius", 0, 0.0, 100.0, 1, 3, NULL);
			uiDefButR(block, UI_BTYPE_NUM, 0, IFACE_("Tilt:"), 0, yi -= buth + but_margin, 200, buth,
			          &data_ptr, "tilt", 0, -tilt_limit, tilt_limit, 1, 3, NULL);
		}
		else if (totcurvedata > 1) {
			uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Mean Weight:"),
			          0, yi -= buth + but_margin, 200, buth,
			          &(tfp->ve_median[C_WEIGHT]), 0.0, 1.0, 1, 3, TIP_("Weight used for Soft Body Goal"));
			uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Mean Radius:"),
			          0, yi -= buth + but_margin, 200, buth,
			          &(tfp->ve_median[C_RADIUS]), 0.0, 100.0, 1, 3, TIP_("Radius of curve control points"));
			but = uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Mean Tilt:"),
			                0, yi -= buth + but_margin, 200, buth,
			                &(tfp->ve_median[C_TILT]), -tilt_limit, tilt_limit, 1, 3,
			                TIP_("Tilt of curve control points"));
			UI_but_unit_type_set(but, PROP_UNIT_ROTATION);
		}
		/* Lattice... */
		else if (totlattdata == 1) {
			uiDefButR(block, UI_BTYPE_NUM, 0, IFACE_("Weight:"), 0, yi -= buth + but_margin, 200, buth,
			          &data_ptr, "weight_softbody", 0, 0.0, 1.0, 1, 3, NULL);
		}
		else if (totlattdata > 1) {
			uiDefButF(block, UI_BTYPE_NUM, B_OBJECTPANELMEDIAN, IFACE_("Mean Weight:"),
			          0, yi -= buth + but_margin, 200, buth,
			          &(tfp->ve_median[L_WEIGHT]), 0.0, 1.0, 1, 3, TIP_("Weight used for Soft Body Goal"));
		}

		UI_block_align_end(block);
	}
	else { /* apply */
		int i;
		bool apply_vcos;

		memcpy(ve_median, tfp->ve_median, sizeof(tfp->ve_median));

		if (v3d->flag & V3D_GLOBAL_STATS) {
			invert_m4_m4(ob->imat, ob->obmat);
			mul_m4_v3(ob->imat, &median[LOC_X]);
			mul_m4_v3(ob->imat, &ve_median[LOC_X]);
		}
		i = NBR_TRANSFORM_PROPERTIES;
		while (i--)
			median[i] = ve_median[i] - median[i];

		/* Note with a single element selected, we always do. */
		apply_vcos = (tot == 1) || (len_squared_v3(&median[LOC_X]) != 0.0f);

		if ((ob->type == OB_MESH) &&
		    (apply_vcos || median[M_BV_WEIGHT] || median[M_SKIN_X] || median[M_SKIN_Y] ||
		     median[M_BE_WEIGHT] || median[M_CREASE]))
		{
			Mesh *me = ob->data;
			BMEditMesh *em = me->edit_btmesh;
			BMesh *bm = em->bm;
			BMIter iter;
			BMVert *eve;
			BMEdge *eed;

			int cd_vert_bweight_offset = -1;
			int cd_vert_skin_offset = -1;
			int cd_edge_bweight_offset = -1;
			int cd_edge_crease_offset = -1;

			float scale_bv_weight = 1.0f;
			float scale_skin_x = 1.0f;
			float scale_skin_y = 1.0f;
			float scale_be_weight = 1.0f;
			float scale_crease = 1.0f;

			/* Vertices */

			if (apply_vcos || median[M_BV_WEIGHT] || median[M_SKIN_X] || median[M_SKIN_Y]) {
				if (median[M_BV_WEIGHT]) {
					BM_mesh_cd_flag_ensure(bm, me, ME_CDFLAG_VERT_BWEIGHT);
					cd_vert_bweight_offset = CustomData_get_offset(&bm->vdata, CD_BWEIGHT);
					BLI_assert(cd_vert_bweight_offset != -1);

					scale_bv_weight = compute_scale_factor(ve_median[M_BV_WEIGHT], median[M_BV_WEIGHT]);
				}

				if (median[M_SKIN_X]) {
					cd_vert_skin_offset = CustomData_get_offset(&bm->vdata, CD_MVERT_SKIN);
					BLI_assert(cd_vert_skin_offset != -1);

					if (ve_median[M_SKIN_X] != median[M_SKIN_X]) {
						scale_skin_x = ve_median[M_SKIN_X] / (ve_median[M_SKIN_X] - median[M_SKIN_X]);
					}
				}
				if (median[M_SKIN_Y]) {
					if (cd_vert_skin_offset == -1) {
						cd_vert_skin_offset = CustomData_get_offset(&bm->vdata, CD_MVERT_SKIN);
						BLI_assert(cd_vert_skin_offset != -1);
					}

					if (ve_median[M_SKIN_Y] != median[M_SKIN_Y]) {
						scale_skin_y = ve_median[M_SKIN_Y] / (ve_median[M_SKIN_Y] - median[M_SKIN_Y]);
					}
				}

				BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
					if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
						if (apply_vcos) {
							apply_raw_diff_v3(eve->co, tot, &ve_median[LOC_X], &median[LOC_X]);
						}

						if (cd_vert_bweight_offset != -1) {
							float *bweight = BM_ELEM_CD_GET_VOID_P(eve, cd_vert_bweight_offset);
							apply_scale_factor_clamp(bweight, tot, ve_median[M_BV_WEIGHT], scale_bv_weight);
						}

						if (cd_vert_skin_offset != -1) {
							MVertSkin *vs = BM_ELEM_CD_GET_VOID_P(eve, cd_vert_skin_offset);

							/* That one is not clamped to [0.0, 1.0]. */
							if (median[M_SKIN_X] != 0.0f) {
								apply_scale_factor(&vs->radius[0], tot, ve_median[M_SKIN_X], median[M_SKIN_X],
								                   scale_skin_x);
							}
							if (median[M_SKIN_Y] != 0.0f) {
								apply_scale_factor(&vs->radius[1], tot, ve_median[M_SKIN_Y], median[M_SKIN_Y],
								                   scale_skin_y);
							}
						}
					}
				}
			}

			if (apply_vcos) {
				EDBM_mesh_normals_update(em);
			}

			/* Edges */

			if (median[M_BE_WEIGHT] || median[M_CREASE]) {
				if (median[M_BE_WEIGHT]) {
					BM_mesh_cd_flag_ensure(bm, me, ME_CDFLAG_EDGE_BWEIGHT);
					cd_edge_bweight_offset = CustomData_get_offset(&bm->edata, CD_BWEIGHT);
					BLI_assert(cd_edge_bweight_offset != -1);

					scale_be_weight = compute_scale_factor(ve_median[M_BE_WEIGHT], median[M_BE_WEIGHT]);
				}

				if (median[M_CREASE]) {
					BM_mesh_cd_flag_ensure(bm, me, ME_CDFLAG_EDGE_CREASE);
					cd_edge_crease_offset = CustomData_get_offset(&bm->edata, CD_CREASE);
					BLI_assert(cd_edge_crease_offset != -1);

					scale_crease = compute_scale_factor(ve_median[M_CREASE], median[M_CREASE]);
				}

				BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
					if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
						if (median[M_BE_WEIGHT] != 0.0f) {
							float *bweight = BM_ELEM_CD_GET_VOID_P(eed, cd_edge_bweight_offset);
							apply_scale_factor_clamp(bweight, tot, ve_median[M_BE_WEIGHT], scale_be_weight);
						}

						if (median[M_CREASE] != 0.0f) {
							float *crease = BM_ELEM_CD_GET_VOID_P(eed, cd_edge_crease_offset);
							apply_scale_factor_clamp(crease, tot, ve_median[M_CREASE], scale_crease);
						}
					}
				}
			}
		}
		else if (ELEM(ob->type, OB_CURVE, OB_SURF) &&
		         (apply_vcos || median[C_BWEIGHT] || median[C_WEIGHT] || median[C_RADIUS] || median[C_TILT]))
		{
			Curve *cu = ob->data;
			Nurb *nu;
			BPoint *bp;
			BezTriple *bezt;
			int a;
			ListBase *nurbs = BKE_curve_editNurbs_get(cu);
			const float scale_w = compute_scale_factor(ve_median[C_WEIGHT], median[C_WEIGHT]);

			nu = nurbs->first;
			while (nu) {
				if (nu->type == CU_BEZIER) {
					for (a = nu->pntsu, bezt = nu->bezt; a--; bezt++) {
						if (bezt->f2 & SELECT) {
							if (apply_vcos) {
								/* Here we always have to use the diff... :/
								 * Cannot avoid some glitches when going e.g. from 3 to 0.0001 (see T37327),
								 * unless we use doubles.
								 */
								add_v3_v3(bezt->vec[0], &median[LOC_X]);
								add_v3_v3(bezt->vec[1], &median[LOC_X]);
								add_v3_v3(bezt->vec[2], &median[LOC_X]);
							}
							if (median[C_WEIGHT]) {
								apply_scale_factor_clamp(&bezt->weight, tot, ve_median[C_WEIGHT], scale_w);
							}
							if (median[C_RADIUS]) {
								apply_raw_diff(&bezt->radius, tot, ve_median[C_RADIUS], median[C_RADIUS]);
							}
							if (median[C_TILT]) {
								apply_raw_diff(&bezt->alfa, tot, ve_median[C_TILT], median[C_TILT]);
							}
						}
						else if (apply_vcos) {  /* Handles can only have their coordinates changed here. */
							if (bezt->f1 & SELECT) {
								apply_raw_diff_v3(bezt->vec[0], tot, &ve_median[LOC_X], &median[LOC_X]);
							}
							if (bezt->f3 & SELECT) {
								apply_raw_diff_v3(bezt->vec[2], tot, &ve_median[LOC_X], &median[LOC_X]);
							}
						}
					}
				}
				else {
					for (a = nu->pntsu * nu->pntsv, bp = nu->bp; a--; bp++) {
						if (bp->f1 & SELECT) {
							if (apply_vcos) {
								apply_raw_diff_v3(bp->vec, tot, &ve_median[LOC_X], &median[LOC_X]);
							}
							if (median[C_BWEIGHT]) {
								apply_raw_diff(&bp->vec[3], tot, ve_median[C_BWEIGHT], median[C_BWEIGHT]);
							}
							if (median[C_WEIGHT]) {
								apply_scale_factor_clamp(&bp->weight, tot, ve_median[C_WEIGHT], scale_w);
							}
							if (median[C_RADIUS]) {
								apply_raw_diff(&bp->radius, tot, ve_median[C_RADIUS], median[C_RADIUS]);
							}
							if (median[C_TILT]) {
								apply_raw_diff(&bp->alfa, tot, ve_median[C_TILT], median[C_TILT]);
							}
						}
					}
				}
				BKE_nurb_test2D(nu);
				BKE_nurb_handles_test(nu, true); /* test for bezier too */

				nu = nu->next;
			}
		}
		else if ((ob->type == OB_LATTICE) && (apply_vcos || median[L_WEIGHT])) {
			Lattice *lt = ob->data;
			BPoint *bp;
			int a;
			const float scale_w = compute_scale_factor(ve_median[L_WEIGHT], median[L_WEIGHT]);

			a = lt->editlatt->latt->pntsu * lt->editlatt->latt->pntsv * lt->editlatt->latt->pntsw;
			bp = lt->editlatt->latt->def;
			while (a--) {
				if (bp->f1 & SELECT) {
					if (apply_vcos) {
						apply_raw_diff_v3(bp->vec, tot, &ve_median[LOC_X], &median[LOC_X]);
					}
					if (median[L_WEIGHT]) {
						apply_scale_factor_clamp(&bp->weight, tot, ve_median[L_WEIGHT], scale_w);
					}
				}
				bp++;
			}
		}

/*		ED_undo_push(C, "Transform properties"); */
	}

/* Clean up! */
/* Location, common to all. */
#undef LOC_X
#undef LOC_Y
#undef LOC_Z
/* Meshes (and lattice)... */
#undef M_BV_WEIGHT
#undef M_SKIN_X
#undef M_SKIN_Y
#undef M_BE_WEIGHT
#undef M_CREASE
/* Curves... */
#undef C_BWEIGHT
#undef C_WEIGHT
#undef C_RADIUS
#undef C_TILT
/* Lattice... */
#undef L_WEIGHT
}
#undef NBR_TRANSFORM_PROPERTIES
#endif

