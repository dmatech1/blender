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
 * The Original Code is Copyright (C) 2013 Blender Foundation.
 * All rights reserved.
 *
 * Original Author: Joshua Leung
 * Contributor(s): Based on original depsgraph.c code - Blender Foundation (2005-2013)
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Methods for constructing depsgraph's nodes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

extern "C" {
#include "BLI_blenlib.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_constraint_types.h"
#include "DNA_curve_types.h"
#include "DNA_effect_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_group_types.h"
#include "DNA_key_types.h"
#include "DNA_lamp_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_node_types.h"
#include "DNA_particle_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"
#include "DNA_world_types.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_animsys.h"
#include "BKE_constraint.h"
#include "BKE_curve.h"
#include "BKE_effect.h"
#include "BKE_fcurve.h"
#include "BKE_group.h"
#include "BKE_key.h"
#include "BKE_lattice.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mesh.h"
#include "BKE_mball.h"
#include "BKE_modifier.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_rigidbody.h"
#include "BKE_sound.h"
#include "BKE_texture.h"
#include "BKE_tracking.h"
#include "BKE_world.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "RNA_access.h"
#include "RNA_types.h"
} /* extern "C" */

#include "depsnode.h"
#include "depsnode_component.h"
#include "depsnode_operation.h"
#include "depsgraph_types.h"
#include "depsgraph_build.h"
#include "depsgraph_eval.h"
#include "depsgraph_intern.h"

#include "stubs.h" // XXX: REMOVE THIS INCLUDE ONCE DEPSGRAPH REFACTOR PROJECT IS DONE!!!

/* ************************************************* */
/* Node Builder */

void DepsgraphNodeBuilder::build_scene(Scene *scene)
{
	/* scene ID block */
	add_id_node(&scene->id);
	
	/* timesource */
	add_time_source(NULL);
	
	/* build subgraph for set, and link this in... */
	// XXX: depending on how this goes, that scene itself could probably store its
	//      own little partial depsgraph?
	if (scene->set) {
		build_scene(scene->set);
	}
	
	/* scene objects */
	for (Base *base = (Base *)scene->base.first; base; base = base->next) {
		Object *ob = base->object;
		
		/* object itself */
		build_object(scene, base, ob);
		
		/* object that this is a proxy for */
		// XXX: the way that proxies work needs to be completely reviewed!
		if (ob->proxy) {
			build_object(scene, base, ob->proxy);
		}
		
		/* handled in next loop... 
		 * NOTE: in most cases, setting dupli-group means that we may want
		 *       to instance existing data and/or reuse it with very few
		 *       modifications...
		 */
		if (ob->dup_group) {
			id_tag_set(&ob->dup_group->id);
		}
	}
	
	/* tagged groups */
	for (Group *group = (Group *)m_bmain->group.first; group; group = (Group *)group->id.next) {
		if (id_is_tagged(&group->id)) {
			// TODO: we need to make this group reliant on the object that spawned it...
			build_subgraph(group);
			
			id_tag_clear(&group->id);
		}
	}
	
	/* rigidbody */
	if (scene->rigidbody_world) {
		build_rigidbody(scene);
	}
	
	/* scene's animation and drivers */
	if (scene->adt) {
		build_animdata(&scene->id);
	}
	
	/* world */
	if (scene->world) {
		build_world(scene->world);
	}
	
	/* compo nodes */
	if (scene->nodetree) {
		build_compositor(scene);
	}
	
	/* sequencer */
	// XXX...
	
	/* grease pencil */
	if (scene->gpd) {
		build_gpencil(scene->gpd);
	}
}

/* Build depsgraph for the given group
 * This is usually used for building subgraphs for groups to use
 */
void DepsgraphNodeBuilder::build_group(Group *group)
{
	/* add group objects */
	for (GroupObject *go = (GroupObject *)group->gobject.first; go; go = go->next) {
		/*Object *ob = go->ob;*/
		
		/* Each "group object" is effectively a separate instance of the underlying
		 * object data. When the group is evaluated, the transform results and/or 
		 * some other attributes end up getting overridden by the group
		 */
	}
}

SubgraphDepsNode *DepsgraphNodeBuilder::build_subgraph(Group *group)
{
	/* sanity checks */
	if (!group)
		return NULL;
	
	/* create new subgraph's data */
	Depsgraph *subgraph = DEG_graph_new();
	
	DepsgraphNodeBuilder subgraph_builder(m_bmain, subgraph);
	subgraph_builder.build_group(group);
	
	/* create a node for representing subgraph */
	SubgraphDepsNode *subgraph_node = m_graph->add_subgraph_node(&group->id);
	subgraph_node->graph = subgraph;
	
	/* make a copy of the data this node will need? */
	// XXX: do we do this now, or later?
	// TODO: need API function which queries graph's ID's hash, and duplicates those blocks thoroughly with all outside links removed...
	
	return subgraph_node;
}

void DepsgraphNodeBuilder::build_object(Scene *scene, Base *base, Object *ob)
{
	IDDepsNode *id_node = add_id_node(&ob->id);
	id_node->layers = base->lay;

	/* standard components */
	build_object_transform(scene, ob);
	
	/* AnimData */
	build_animdata(&ob->id);
	

	/* object data */
	if (ob->data) {
		ID *obdata = (ID *)ob->data;
		/* ob data animation */
		build_animdata(obdata);
		
		/* type-specific data... */
		switch (ob->type) {
			case OB_MESH:     /* Geometry */
			case OB_CURVE:
			case OB_FONT:
			case OB_SURF:
			case OB_MBALL:
			case OB_LATTICE:
			{
				build_obdata_geom(scene, ob);
			}
			break;
			
			case OB_ARMATURE: /* Pose */
				build_rig(scene, ob);
				break;
			
			case OB_LAMP:   /* Lamp */
				build_lamp(ob);
				break;
				
			case OB_CAMERA: /* Camera */
				build_camera(ob);
				break;
		}
	}
	
	/* particle systems */
	if (ob->particlesystem.first) {
		build_particles(ob);
	}
	
	/* grease pencil */
	if (ob->gpd) {
		build_gpencil(ob->gpd);
	}
}

void DepsgraphNodeBuilder::build_object_transform(Scene *scene, Object *ob)
{
	/* local transforms (from transform channels - loc/rot/scale + deltas) */
	add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
	                   DEPSOP_TYPE_INIT, function_bind(BKE_object_eval_local_transform, _1, scene, ob),
	                   DEG_OPCODE_TRANSFORM_LOCAL);
					   
	/* object parent */
	if (ob->parent) {
		add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
		                   DEPSOP_TYPE_EXEC, function_bind(BKE_object_eval_parent, _1, scene, ob),
		                   DEG_OPCODE_TRANSFORM_PARENT);
	}
	
	/* object constraints */
	if (ob->constraints.first) {
		build_object_constraints(scene, ob);
	}

	/* Temporary uber-update node, which does everything.
	 * It is for the being we're porting old dependencies into the new system.
	 * We'll get rid of this node as soon as all the granular update functions
	 * are filled in.
	 *
	 * TODO(sergey): Get rid of this node.
	 */
	add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
	                   DEPSOP_TYPE_EXEC, function_bind(BKE_object_eval_uber_transform, _1, scene, ob),
	                   DEG_OPCODE_OBJECT_UBEREVAL);
	
	/* object transform is done */
	add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
	                   DEPSOP_TYPE_POST, function_bind(BKE_object_eval_done, _1, ob),
	                   DEG_OPCODE_TRANSFORM_FINAL);
}

/* == Constraints Graph Notes ==
 * For constraints, we currently only add a operation node to the Transform
 * or Bone components (depending on whichever type of owner we have).
 * This represents the entire constraints stack, which is for now just
 * executed as a single monolithic block. At least initially, this should
 * be sufficient for ensuring that the porting/refactoring process remains
 * manageable. 
 * 
 * However, when the time comes for developing "node-based" constraints,
 * we'll need to split this up into pre/post nodes for "constraint stack
 * evaluation" + operation nodes for each constraint (i.e. the contents
 * of the loop body used in the current "solve_constraints()" operation).
 *
 * -- Aligorith, August 2013 
 */
void DepsgraphNodeBuilder::build_object_constraints(Scene *scene, Object *ob)
{
	/* create node for constraint stack */
	add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
	                   DEPSOP_TYPE_EXEC, function_bind(BKE_object_eval_constraints, _1, scene, ob),
	                   DEG_OPCODE_TRANSFORM_CONSTRAINTS);
}

void DepsgraphNodeBuilder::build_pose_constraints(Object *ob, bPoseChannel *pchan)
{
	/* create node for constraint stack */
	add_operation_node(&ob->id, DEPSNODE_TYPE_BONE, pchan->name,
	                   DEPSOP_TYPE_EXEC, function_bind(BKE_pose_constraints_evaluate, _1, ob, pchan),
	                   DEG_OPCODE_BONE_CONSTRAINTS);
}

/* Build graph nodes for AnimData block 
 * < scene_node: Scene that ID-block this lives on belongs to
 * < id: ID-Block which hosts the AnimData
 */
void DepsgraphNodeBuilder::build_animdata(ID *id)
{
	AnimData *adt = BKE_animdata_from_id(id);
	
	if (adt == NULL)
		return;
	
	/* animation */
	if (adt->action || adt->nla_tracks.first || adt->drivers.first) {
		TimeSourceDepsNode *time_src = m_graph->find_time_source();
		
		// XXX: Hook up specific update callbacks for special properties which may need it...
		
		/* actions and NLA - as a single unit for now, as it gets complicated to schedule otherwise */
		if ((adt->action) || (adt->nla_tracks.first)) {
			/* create the node */
			add_operation_node(id, DEPSNODE_TYPE_ANIMATION,
			                   DEPSOP_TYPE_EXEC, function_bind(BKE_animsys_eval_animdata, _1, id, time_src),
			                   DEG_OPCODE_ANIMATION, id->name);
			
			// TODO: for each channel affected, we might also want to add some support for running RNA update callbacks on them
			// (which will be needed for proper handling of drivers later)
		}
		
		/* drivers */
		for (FCurve *fcu = (FCurve *)adt->drivers.first; fcu; fcu = fcu->next) {
			/* create driver */
			build_driver(id, fcu);
		}
	}
}

/* Build graph node(s) for Driver
 * < id: ID-Block that driver is attached to
 * < fcu: Driver-FCurve
 */
OperationDepsNode *DepsgraphNodeBuilder::build_driver(ID *id, FCurve *fcu)
{
	ChannelDriver *driver = fcu->driver;
	
	/* create data node for this driver ..................................... */
	TimeSourceDepsNode *time_src = m_graph->find_time_source();
	OperationDepsNode *driver_op = add_operation_node(id, DEPSNODE_TYPE_PARAMETERS,
	                                                  DEPSOP_TYPE_EXEC, function_bind(BKE_animsys_eval_driver, _1, id, fcu, time_src),
	                                                  DEG_OPCODE_DRIVER, deg_fcurve_id_name(fcu));
	
	/* tag "scripted expression" drivers as needing Python (due to GIL issues, etc.) */
	if (driver->type == DRIVER_TYPE_PYTHON) {
		driver_op->flag |= DEPSOP_FLAG_USES_PYTHON;
	}
	
	/* return driver node created */
	return driver_op;
}

/* Recursively build graph for world */
void DepsgraphNodeBuilder::build_world(World *world)
{
	/* Prevent infinite recursion by checking (and tagging the world) as having been visited 
	 * already. This assumes wo->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *world_id = &world->id;
	if (id_is_tagged(world_id))
		return;
	id_tag_set(world_id);
	
	/* world itself */
	IDDepsNode *world_node = add_id_node(world_id); /* world shading/params? */
	
	build_animdata(world_id);
	
	/* TODO: other settings? */
	
	/* textures */
	build_texture_stack(world_node, world->mtex);
	
	/* world's nodetree */
	if (world->nodetree) {
		build_nodetree(world_node, world->nodetree);
	}

	id_tag_clear(world_id);
}

/* Rigidbody Simulation - Scene Level */
void DepsgraphNodeBuilder::build_rigidbody(Scene *scene)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	
	/* == Rigidbody Simulation Nodes == 
	 * There are 3 nodes related to Rigidbody Simulation:
	 * 1) "Initialise/Rebuild World" - this is called sparingly, only when the simulation
	 *    needs to be rebuilt (mainly after file reload, or moving back to start frame)
	 * 2) "Do Simulation" - perform a simulation step - interleaved between the evaluation
	 *    steps for clusters of objects (i.e. between those affected and/or not affected by
	 *    the sim for instance)
	 *
	 * 3) "Pull Results" - grab the specific transforms applied for a specific object -
	 *    performed as part of object's transform-stack building
	 */
	
	/* create nodes ------------------------------------------------------------------------ */
	/* XXX: is this the right component, or do we want to use another one instead? */
	
	/* init/rebuild operation */
	/*OperationDepsNode *init_node =*/ add_operation_node(&scene->id, DEPSNODE_TYPE_TRANSFORM,
	                                                      DEPSOP_TYPE_REBUILD, function_bind(BKE_rigidbody_rebuild_sim, _1, scene),
	                                                      DEG_OPCODE_RIGIDBODY_REBUILD);
	
	/* do-sim operation */
	// XXX: what happens if we need to split into several groups?
	OperationDepsNode *sim_node     = add_operation_node(&scene->id, DEPSNODE_TYPE_TRANSFORM,
	                                                     DEPSOP_TYPE_SIM, function_bind(BKE_rigidbody_eval_simulation, _1, scene),
	                                                     DEG_OPCODE_RIGIDBODY_SIM);
	
	/* XXX: For now, the sim node is the only one that really matters here. If any other
	 * sims get added later, we may have to remove these hacks... 
	 */
	sim_node->owner->entry_operation = sim_node;
	sim_node->owner->exit_operation  = sim_node;
	
	
	/* objects - simulation participants */
	if (rbw->group) {
		for (GroupObject *go = (GroupObject *)rbw->group->gobject.first; go; go = go->next) {
			Object *ob = go->ob;
			
			if (!ob || (ob->type != OB_MESH))
				continue;
			
			/* 2) create operation for flushing results */
			/* object's transform component - where the rigidbody operation lives */
			add_operation_node(&ob->id, DEPSNODE_TYPE_TRANSFORM,
			                   DEPSOP_TYPE_EXEC, function_bind(BKE_rigidbody_object_sync_transforms, _1, scene, ob),
			                   DEG_OPCODE_TRANSFORM_RIGIDBODY);
		}
	}
}

void DepsgraphNodeBuilder::build_particles(Object *ob)
{
	/* == Particle Systems Nodes ==
	 * There are two types of nodes associated with representing
	 * particle systems:
	 *  1) Component (EVAL_PARTICLES) - This is the particle-system
	 *     evaluation context for an object. It acts as the container
	 *     for all the nodes associated with a particular set of particle
	 *     systems.
	 *  2) Particle System Eval Operation - This operation node acts as a
	 *     blackbox evaluation step for one particle system referenced by
	 *     the particle systems stack. All dependencies link to this operation.
	 */
	
	/* component for all particle systems */
	ComponentDepsNode *psys_comp = add_component_node(&ob->id, DEPSNODE_TYPE_EVAL_PARTICLES);
	
	/* particle systems */
	for (ParticleSystem *psys = (ParticleSystem *)ob->particlesystem.first; psys; psys = psys->next) {
		ParticleSettings *part = psys->part;
		
		/* particle settings */
		// XXX: what if this is used more than once!
		build_animdata(&part->id);
		
		/* this particle system */
		// TODO: for now, this will just be a placeholder "ubereval" node
		add_operation_node(psys_comp,
		                   DEPSOP_TYPE_EXEC, function_bind(BKE_particle_system_eval, _1, ob, psys),
		                   DEG_OPCODE_PSYS_EVAL);
	}
	
	/* pointcache */
	// TODO...
}

/* IK Solver Eval Steps */
void DepsgraphNodeBuilder::build_ik_pose(Scene *scene, Object *ob, bPoseChannel *pchan, bConstraint *con)
{
	bKinematicConstraint *data = (bKinematicConstraint *)con->data;
	
	/* Find the chain's root. */
	bPoseChannel *rootchan = BKE_armature_ik_solver_find_root(pchan, data);
	
	/* Operation node for evaluating/running IK Solver. */
	add_operation_node(&ob->id, DEPSNODE_TYPE_EVAL_POSE, rootchan->name,
	                   DEPSOP_TYPE_SIM, function_bind(BKE_pose_iktree_evaluate, _1, scene, ob, rootchan),
	                   DEG_OPCODE_POSE_IK_SOLVER);
}

/* Spline IK Eval Steps */
void DepsgraphNodeBuilder::build_splineik_pose(Scene *scene, Object *ob, bPoseChannel *pchan, bConstraint *con)
{
	bSplineIKConstraint *data = (bSplineIKConstraint *)con->data;
	
	/* Find the chain's root. */
	bPoseChannel *rootchan = BKE_armature_splineik_solver_find_root(pchan, data);
	
	/* Operation node for evaluating/running Spline IK Solver.
	 * Store the "root bone" of this chain in the solver, so it knows where to start.
	 */
	add_operation_node(&ob->id, DEPSNODE_TYPE_EVAL_POSE, rootchan->name,
	                   DEPSOP_TYPE_SIM, function_bind(BKE_pose_splineik_evaluate, _1, scene, ob, rootchan),
	                   DEG_OPCODE_POSE_SPLINE_IK_SOLVER);
}

/* Pose/Armature Bones Graph */
void DepsgraphNodeBuilder::build_rig(Scene *scene, Object *ob)
{
	bArmature *arm = (bArmature *)ob->data;

	/* animation and/or drivers linking posebones to base-armature used to define them 
	 * NOTE: AnimData here is really used to control animated deform properties, 
	 *       which ideally should be able to be unique across different instances.
	 *       Eventually, we need some type of proxy/isolation mechanism inbetween here
	 *       to ensure that we can use same rig multiple times in same scene...
	 *
	 *       However, since the armature is obdata, it's drivers have already been added
	 *       before this function was called, so nothing needs to happen here...
	 */
	/* build_animdata(&arm->id); */

	
	/* Rebuild pose if not up to date. */
	if (ob->pose == NULL || (ob->pose->flag & POSE_RECALC)) {
		BKE_pose_rebuild(ob, arm);
	}

	
	/* == Pose Rig Graph ==
	 * Pose Component:
	 * - Mainly used for referencing Bone components.
	 * - This is where the evaluation operations for init/exec/cleanup
	 *   (ik) solvers live, and are later hooked up (so that they can be
	 *   interleaved during runtime) with bone-operations they depend on/affect.
	 * - init_pose_eval() and cleanup_pose_eval() are absolute first and last
	 *   steps of pose eval process. ALL bone operations must be performed 
	 *   between these two...
	 * 
	 * Bone Component:
	 * - Used for representing each bone within the rig
	 * - Acts to encapsulate the evaluation operations (base matrix + parenting, 
	 *   and constraint stack) so that they can be easily found.
	 * - Everything else which depends on bone-results hook up to the component only
	 *   so that we can redirect those to point at either the the post-IK/
	 *   post-constraint/post-matrix steps, as needed.
	 */
	
	/* pose eval context */
	add_operation_node(&ob->id, DEPSNODE_TYPE_EVAL_POSE,
	                   DEPSOP_TYPE_INIT, function_bind(BKE_pose_eval_init, _1, scene, ob, ob->pose), DEG_OPCODE_POSE_INIT);
	
	add_operation_node(&ob->id, DEPSNODE_TYPE_EVAL_POSE,
	                   DEPSOP_TYPE_POST, function_bind(BKE_pose_eval_flush, _1, scene, ob, ob->pose), DEG_OPCODE_POSE_DONE);
	
	/* bones */
	for (bPoseChannel *pchan = (bPoseChannel *)ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		/* node for bone eval */
		add_operation_node(&ob->id, DEPSNODE_TYPE_BONE, pchan->name,
		                   DEPSOP_TYPE_INIT, NULL, // XXX: BKE_pose_eval_bone_local
		                   DEG_OPCODE_BONE_LOCAL);
		
		add_operation_node(&ob->id, DEPSNODE_TYPE_BONE, pchan->name,
		                   DEPSOP_TYPE_EXEC, function_bind(BKE_pose_eval_bone, _1, scene, ob, pchan), // XXX: BKE_pose_eval_bone_pose
		                   DEG_OPCODE_BONE_POSE_PARENT);
						   
		add_operation_node(&ob->id, DEPSNODE_TYPE_BONE, pchan->name,
		                   DEPSOP_TYPE_OUT, NULL, /* NOTE: dedicated noop for easier relationship construction */
		                   DEG_OPCODE_BONE_READY);
		
		add_operation_node(&ob->id, DEPSNODE_TYPE_BONE, pchan->name,
		                   DEPSOP_TYPE_POST, function_bind(BKE_pose_bone_done, _1, pchan),
		                   DEG_OPCODE_BONE_DONE);
		
		/* constraints */
		if (pchan->constraints.first != NULL) {
			build_pose_constraints(ob, pchan);
		}
		
		/* IK Solvers...
		 * - These require separate processing steps are pose-level
		 *   to be executed between chains of bones (i.e. once the
		 *   base transforms of a bunch of bones is done)
		 *
		 * Unsolved Issues:
		 * - Care is needed to ensure that multi-headed trees work out the same as in ik-tree building
		 * - Animated chain-lengths are a problem...
		 */
		for (bConstraint *con = (bConstraint *)pchan->constraints.first; con; con = con->next) {
			switch (con->type) {
				case CONSTRAINT_TYPE_KINEMATIC:
					build_ik_pose(scene, ob, pchan, con);
					break;
					
				case CONSTRAINT_TYPE_SPLINEIK:
					build_splineik_pose(scene, ob, pchan, con);
					break;
					
				default:
					break;
			}
		}
	}
}

/* Shapekeys */
void DepsgraphNodeBuilder::build_shapekeys(Key *key)
{
	build_animdata(&key->id);
}

/* ObData Geometry Evaluation */
// XXX: what happens if the datablock is shared!
void DepsgraphNodeBuilder::build_obdata_geom(Scene *scene, Object *ob)
{
	ID *obdata = (ID *)ob->data;
	
	/* Temporary uber-update node, which does everything.
	 * It is for the being we're porting old dependencies into the new system.
	 * We'll get rid of this node as soon as all the granular update functions
	 * are filled in.
	 *
	 * TODO(sergey): Get rid of this node.
	 */
	add_operation_node(&ob->id, DEPSNODE_TYPE_GEOMETRY,
					   DEPSOP_TYPE_POST, function_bind(BKE_object_eval_uber_data, _1, scene, ob),
					   DEG_OPCODE_GEOMETRY_UBEREVAL);
	
	/* nodes for result of obdata's evaluation, and geometry evaluation on object */
	switch (ob->type) {
		case OB_MESH:
		{
			//Mesh *me = (Mesh *)ob->data;
			
			/* evaluation operations */
			add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_INIT, function_bind(BKE_mesh_eval_geometry, _1, (Mesh *)obdata),
			                   DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
		}
		break;
		
		case OB_MBALL: 
		{
			Object *mom = BKE_mball_basis_find(scene, ob);
			
			/* motherball - mom depends on children! */
			if (mom == ob) {
				/* metaball evaluation operations */
				/* NOTE: only the motherball gets evaluated! */
				add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
				                   DEPSOP_TYPE_INIT, function_bind(BKE_mball_eval_geometry, _1, (MetaBall *)obdata),
				                   DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
			}
		}
		break;
		
		case OB_CURVE:
		case OB_FONT:
		{
			/* curve evaluation operations */
			/* - calculate curve geometry (including path) */
			add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_INIT, function_bind(BKE_curve_eval_geometry, _1, (Curve *)obdata),
			                   DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
			
			/* - calculate curve path - this is used by constraints, etc. */
			add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_EXEC, function_bind(BKE_curve_eval_path, _1, (Curve *)obdata),
			                   DEG_OPCODE_GEOMETRY_PATH, "Path");
		}
		break;
		
		case OB_SURF: /* Nurbs Surface */
		{
			/* nurbs evaluation operations */
			add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_INIT, function_bind(BKE_curve_eval_geometry, _1, (Curve *)obdata),
			                   DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
		}
		break;
		
		case OB_LATTICE: /* Lattice */
		{
			/* lattice evaluation operations */
			add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_INIT, function_bind(BKE_lattice_eval_geometry, _1, (Lattice *)obdata),
			                   DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
		}
		break;
	}

	add_operation_node(obdata, DEPSNODE_TYPE_GEOMETRY,
	                   DEPSOP_TYPE_POST, NULL,
	                   DEG_OPCODE_PLACEHOLDER, "Eval Done");

	add_operation_node(&ob->id, DEPSNODE_TYPE_GEOMETRY,
	                   DEPSOP_TYPE_INIT, NULL,
	                   DEG_OPCODE_PLACEHOLDER, "Eval Init");

	// TODO: "Done" operation
	
	/* ShapeKeys */
	Key *key = BKE_key_from_object(ob);
	if (key)
		build_shapekeys(key);
	
	/* Modifiers */
	if (ob->modifiers.first) {
		ModifierData *md;
		
		for (md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
			add_operation_node(&ob->id, DEPSNODE_TYPE_GEOMETRY,
			                   DEPSOP_TYPE_EXEC, function_bind(BKE_object_eval_modifier, _1, scene, ob, md),
			                   DEG_OPCODE_GEOMETRY_MODIFIER, md->name);
		}
	}
	
	/* materials */
	if (ob->totcol) {
		int a;
		
		for (a = 1; a <= ob->totcol; a++) {
			Material *ma = give_current_material(ob, a);
			
			if (ma) {
				// XXX?!
				ComponentDepsNode *geom_node = add_component_node(&ob->id, DEPSNODE_TYPE_GEOMETRY);
				build_material(geom_node, ma);
			}
		}
	}
	
	/* geometry collision */
	if (ELEM(ob->type, OB_MESH, OB_CURVE, OB_LATTICE)) {
		// add geometry collider relations
	}
}

/* Cameras */
void DepsgraphNodeBuilder::build_camera(Object *ob)
{
	/* TODO: Link scene-camera links in somehow... */
}

/* Lamps */
void DepsgraphNodeBuilder::build_lamp(Object *ob)
{
	Lamp *la = (Lamp *)ob->data;
	ID *lamp_id = &la->id;
	
	/* Prevent infinite recursion by checking (and tagging the lamp) as having been visited 
	 * already. This assumes la->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	if (id_is_tagged(lamp_id))
		return;
	id_tag_set(lamp_id);
	
	/* node for obdata */
	ComponentDepsNode *param_node = add_component_node(lamp_id, DEPSNODE_TYPE_PARAMETERS);
	
	/* lamp's nodetree */
	if (la->nodetree) {
		build_nodetree(param_node, la->nodetree);
	}
	
	/* textures */
	build_texture_stack(param_node, la->mtex);
	
	id_tag_clear(lamp_id);
}

void DepsgraphNodeBuilder::build_nodetree(DepsNode *owner_node, bNodeTree *ntree)
{
	if (!ntree)
		return;
	
	/* nodetree itself */
	ID *ntree_id = &ntree->id;
	build_animdata(ntree_id);
	
	/* nodetree's nodes... */
	for (bNode *bnode = (bNode *)ntree->nodes.first; bnode; bnode = bnode->next) {
		if (bnode->id) {
			if (GS(bnode->id->name) == ID_MA) {
				build_material(owner_node, (Material *)bnode->id);
			}
			else if (bnode->type == ID_TE) {
				build_texture(owner_node, (Tex *)bnode->id);
			}
			else if (bnode->type == NODE_GROUP) {
				build_nodetree(owner_node, (bNodeTree *)bnode->id);
			}
		}
	}
	
	// TODO: link from nodetree to owner_component?
}

/* Recursively build graph for material */
void DepsgraphNodeBuilder::build_material(DepsNode *owner_node, Material *ma)
{
	/* Prevent infinite recursion by checking (and tagging the material) as having been visited 
	 * already. This assumes ma->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *ma_id = &ma->id;
	if (id_is_tagged(ma_id))
		return;
	id_tag_set(ma_id);

	/* material itself */
	add_id_node(ma_id);

	add_operation_node(ma_id, DEPSNODE_TYPE_SHADING,
	                   DEPSOP_TYPE_EXEC, NULL,
	                   DEG_OPCODE_PLACEHOLDER, "Material Update");

	/* material animation */
	build_animdata(ma_id);
	
	/* textures */
	build_texture_stack(owner_node, ma->mtex);
	
	/* material's nodetree */
	build_nodetree(owner_node, ma->nodetree);
	
	id_tag_clear(ma_id);
}

/* Texture-stack attached to some shading datablock */
void DepsgraphNodeBuilder::build_texture_stack(DepsNode *owner_node, MTex **texture_stack)
{
	int i;
	
	/* for now assume that all texture-stacks have same number of max items */
	for (i = 0; i < MAX_MTEX; i++) {
		MTex *mtex = texture_stack[i];
		if (mtex && mtex->tex)
			build_texture(owner_node, mtex->tex);
	}
}

/* Recursively build graph for texture */
void DepsgraphNodeBuilder::build_texture(DepsNode *owner_node, Tex *tex)
{
	/* Prevent infinite recursion by checking (and tagging the texture) as having been visited 
	 * already. This assumes tex->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *tex_id = &tex->id;
	if (id_is_tagged(tex_id))
		return;
	id_tag_set(tex_id);
	
	/* texture itself */
	build_animdata(tex_id);
	
	/* texture's nodetree */
	build_nodetree(owner_node, tex->nodetree);
	
	id_tag_clear(tex_id);
}

void DepsgraphNodeBuilder::build_compositor(Scene *scene)
{
	/* For now, just a plain wrapper? */
	// TODO: create compositing component?
	// XXX: component type undefined!
	//graph->get_node(&scene->id, NULL, DEPSNODE_TYPE_COMPOSITING, NULL);
	
	/* for now, nodetrees are just parameters; compositing occurs in internals of renderer... */
	ComponentDepsNode *owner_node = add_component_node(&scene->id, DEPSNODE_TYPE_PARAMETERS);
	build_nodetree(owner_node, scene->nodetree);
}

void DepsgraphNodeBuilder::build_gpencil(bGPdata *gpd)
{
	ID *gpd_id = &gpd->id;
	
	/* gpencil itself */
	// XXX: what about multiple users of same datablock? This should only get added once
	add_id_node(gpd_id);
	
	/* The main reason Grease Pencil is included here is because the animation (and drivers)
	 * need to be hosted somewhere...
	 */
	build_animdata(gpd_id);
}
