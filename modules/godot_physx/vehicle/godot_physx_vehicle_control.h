/**************************************************************************/
/*  godot_physx_vehicle_control.h                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/


#pragma once

// RigidBody3D-style control of a PhysX vehicle's chassis, shared by
// PhysXVehicle3D, PhysXTank3D and PhysXMotorcycle3D: forces, torques and
// impulses (world space; a force's position is an offset from the chassis
// origin in world axes, as RigidBody3D's), velocities, a teleport that keeps
// the vehicle (no rebuild), and a live tire grip multiplier.
//
// A class declares the methods with PHYSX_VEHICLE_CONTROL_DECLARE() and
// defines / binds them with PHYSX_VEHICLE_CONTROL_DEFINE / _BIND. Its Impl
// needs `built` and `vehicle` (with physxActor, physxMaterialFrictionParams,
// wheelRigidBody1dStates, tireStickyStates); m_wheel_count is an expression for
// how many wheels the vehicle has. Call _store_tire_grip_base() once the vehicle
// is configured, so tire_grip scales the tires' own friction.

#include "../godot_physx_conversions.h"

#include "core/math/transform_3d.h"
#include "core/object/class_db.h"

#include <PxPhysicsAPI.h>

#define PHYSX_VEHICLE_CONTROL_DECLARE()                                                         \
public:                                                                                         \
	void apply_central_force(const Vector3 &p_force);                                           \
	void apply_force(const Vector3 &p_force, const Vector3 &p_position = Vector3());            \
	void apply_torque(const Vector3 &p_torque);                                                 \
	void apply_central_impulse(const Vector3 &p_impulse);                                       \
	void apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position = Vector3());        \
	void apply_torque_impulse(const Vector3 &p_impulse);                                        \
	void set_linear_velocity(const Vector3 &p_velocity);                                        \
	void set_angular_velocity(const Vector3 &p_velocity);                                       \
	Vector3 get_angular_velocity() const;                                                       \
	void teleport(const Transform3D &p_transform, bool p_keep_velocity = false);                \
	void set_tire_grip(real_t p_grip);                                                          \
	real_t get_tire_grip() const { return tire_grip; }                                          \
                                                                                                \
private:                                                                                        \
	/* Live multiplier on the tires' friction (terrain grip); the base values */              \
	/* are the tires' own, stored when the vehicle is built. */                                \
	real_t tire_grip = 1.0;                                                                     \
	float tire_base_friction[32] = {};                                                          \
	physx::PxRigidDynamic *_control_body() const;                                               \
	void _store_tire_grip_base();                                                               \
	void _apply_tire_grip();                                                                    \
                                                                                                \
public:

#define PHYSX_VEHICLE_CONTROL_DEFINE(m_class, m_wheel_count)                                                                      \
	physx::PxRigidDynamic *m_class::_control_body() const {                                                                       \
		return impl->built ? impl->vehicle.physxActor.rigidBody->is<physx::PxRigidDynamic>() : nullptr;                           \
	}                                                                                                                             \
	void m_class::apply_central_force(const Vector3 &p_force) {                                                                   \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->addForce(to_px(p_force), physx::PxForceMode::eFORCE);                                                            \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::apply_force(const Vector3 &p_force, const Vector3 &p_position) {                                                \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			physx::PxRigidBodyExt::addForceAtPos(*b, to_px(p_force), b->getGlobalPose().p + to_px(p_position), physx::PxForceMode::eFORCE); \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::apply_torque(const Vector3 &p_torque) {                                                                         \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->addTorque(to_px(p_torque), physx::PxForceMode::eFORCE);                                                          \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::apply_central_impulse(const Vector3 &p_impulse) {                                                               \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->addForce(to_px(p_impulse), physx::PxForceMode::eIMPULSE);                                                        \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position) {                                            \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			physx::PxRigidBodyExt::addForceAtPos(*b, to_px(p_impulse), b->getGlobalPose().p + to_px(p_position), physx::PxForceMode::eIMPULSE); \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::apply_torque_impulse(const Vector3 &p_impulse) {                                                                \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->addTorque(to_px(p_impulse), physx::PxForceMode::eIMPULSE);                                                       \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::set_linear_velocity(const Vector3 &p_velocity) {                                                                \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->setLinearVelocity(to_px(p_velocity));                                                                            \
		}                                                                                                                         \
	}                                                                                                                             \
	void m_class::set_angular_velocity(const Vector3 &p_velocity) {                                                               \
		if (physx::PxRigidDynamic *b = _control_body()) {                                                                        \
			b->setAngularVelocity(to_px(p_velocity));                                                                           \
		}                                                                                                                         \
	}                                                                                                                             \
	Vector3 m_class::get_angular_velocity() const {                                                                               \
		physx::PxRigidDynamic *b = _control_body();                                                                              \
		return b != nullptr ? to_godot(b->getAngularVelocity()) : Vector3();                                                     \
	}                                                                                                                             \
	void m_class::teleport(const Transform3D &p_transform, bool p_keep_velocity) {                                                \
		physx::PxRigidDynamic *b = _control_body();                                                                              \
		if (b == nullptr) {                                                                                                       \
			set_global_transform(p_transform);                                                                                    \
			return;                                                                                                               \
		}                                                                                                                         \
		/* The chassis moves in place: same actor, wheels and settings, nothing rebuilt. */                                      \
		b->setGlobalPose(to_px(p_transform));                                                                                     \
		if (!p_keep_velocity) {                                                                                                   \
			b->setLinearVelocity(physx::PxVec3(0.0f));                                                                           \
			b->setAngularVelocity(physx::PxVec3(0.0f));                                                                          \
			/* And the wheels stop spinning; no tire stays "stuck" from before. */                                              \
			for (uint32_t i = 0; i < (uint32_t)(m_wheel_count); i++) {                                                            \
				impl->vehicle.wheelRigidBody1dStates[i].setToDefault();                                                           \
				impl->vehicle.tireStickyStates[i].setToDefault();                                                                 \
			}                                                                                                                     \
		}                                                                                                                         \
		b->wakeUp();                                                                                                              \
		set_global_transform(to_godot(b->getGlobalPose()));                                                                       \
	}                                                                                                                             \
	void m_class::set_tire_grip(real_t p_grip) {                                                                                  \
		tire_grip = MAX(p_grip, (real_t)0.0);                                                                                     \
		_apply_tire_grip();                                                                                                       \
	}                                                                                                                             \
	void m_class::_store_tire_grip_base() {                                                                                       \
		for (uint32_t i = 0; i < (uint32_t)(m_wheel_count) && i < 32; i++) {                                                      \
			tire_base_friction[i] = impl->vehicle.physxMaterialFrictionParams[i].defaultFriction;                                 \
		}                                                                                                                         \
		_apply_tire_grip();                                                                                                       \
	}                                                                                                                             \
	void m_class::_apply_tire_grip() {                                                                                            \
		if (!impl->built) {                                                                                                       \
			return;                                                                                                               \
		}                                                                                                                         \
		/* The road friction the tire model scales each tire's grip by -- read every step, so */                                \
		/* this takes effect at once (the wheels' tire_* properties rebuild the vehicle). */                                    \
		for (uint32_t i = 0; i < (uint32_t)(m_wheel_count) && i < 32; i++) {                                                      \
			impl->vehicle.physxMaterialFrictionParams[i].defaultFriction = tire_base_friction[i] * (float)tire_grip;              \
		}                                                                                                                         \
	}

#define PHYSX_VEHICLE_CONTROL_BIND(m_class)                                                                                                    \
	ClassDB::bind_method(D_METHOD("apply_central_force", "force"), &m_class::apply_central_force);                                           \
	ClassDB::bind_method(D_METHOD("apply_force", "force", "position"), &m_class::apply_force, DEFVAL(Vector3()));                              \
	ClassDB::bind_method(D_METHOD("apply_torque", "torque"), &m_class::apply_torque);                                                       \
	ClassDB::bind_method(D_METHOD("apply_central_impulse", "impulse"), &m_class::apply_central_impulse);                                     \
	ClassDB::bind_method(D_METHOD("apply_impulse", "impulse", "position"), &m_class::apply_impulse, DEFVAL(Vector3()));                       \
	ClassDB::bind_method(D_METHOD("apply_torque_impulse", "impulse"), &m_class::apply_torque_impulse);                                       \
	ClassDB::bind_method(D_METHOD("set_linear_velocity", "velocity"), &m_class::set_linear_velocity);                                        \
	ClassDB::bind_method(D_METHOD("set_angular_velocity", "velocity"), &m_class::set_angular_velocity);                                      \
	ClassDB::bind_method(D_METHOD("get_angular_velocity"), &m_class::get_angular_velocity);                                                  \
	ClassDB::bind_method(D_METHOD("teleport", "transform", "keep_velocity"), &m_class::teleport, DEFVAL(false));                              \
	ClassDB::bind_method(D_METHOD("set_tire_grip", "grip"), &m_class::set_tire_grip);                                                        \
	ClassDB::bind_method(D_METHOD("get_tire_grip"), &m_class::get_tire_grip);                                                                \
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_grip", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_tire_grip", "get_tire_grip");
