#pragma once
#include <cstdint>

// Field offsets for the trailer connect/disconnect feature, re-derived for game
// version 1.61.1.9 (ETS2 and ATS share them). The structs in prism/ still describe
// the 1.53-era layout and are NOT valid for these fields anymore, so read them
// through here instead.
//
// 1.61.1.9 added 0x28 bytes to the trailer object ahead of these fields, so every
// offset past that point moved from the 1.61.1.1 values (slave 0x1060 -> 0x1088).
// game_actor itself is unchanged.
//
// How each was found (offline, from the exe):
//  - slave trailer: prism::physics_trailer_u::connect_slave tests +0x1088 against null
//  - first trailer: the `cheat repair` handler walks game_actor +0x18 (truck) and +0xC8
namespace ts_extra_utilities::prism::offsets
{
    constexpr uint32_t game_actor_truck = 0x0018; // game_physics_vehicle_u*, the player's truck
    constexpr uint32_t game_actor_trailer = 0x00C8; // game_trailer_actor_u*, first trailer
    constexpr uint32_t trailer_parent = 0x1080; // game_trailer_actor_u*, previous trailer in the chain
    constexpr uint32_t trailer_slave = 0x1088; // game_trailer_actor_u*, next trailer in the chain

    // Trailer steering (1.61.1.9): physics_trailer_u::steering_advance stores the
    // computed angle to +0x6B8 and passes the pointer at +0x6C0 to
    // set_individual_steering; the data object is null on trailers without
    // steerable wheels
    constexpr uint32_t trailer_steering = 0x06B8; // float, -1..1, written by the game every advance
    constexpr uint32_t trailer_steering_data = 0x06C0; // vehicle_wheel_steering_data_t*, null when not steerable

    // Trailer connect/disconnect (1.61.1.9). physics_joint is null while a trailer is
    // detached; the game's own detach path checks it before calling disconnect and then
    // lowering the brace. hook_locator is the vehicle's own coupling point in local
    // space; chassis+0x740 is the rear hook position on the chassis data object
    // (truck+0x200). All verified in the native attach/detach code.
    constexpr uint32_t trailer_physics_joint = 0x0FE0; // physics_joint_physx_t*, null while detached
    constexpr uint32_t vehicle_hook_locator = 0x0464; // float3_t
    constexpr uint32_t vehicle_chassis_data = 0x0200; // chassis data object
    constexpr uint32_t chassis_hook_position = 0x0740; // float3_t

    // Parked-trailer state: the game sets trailer_parked once a jointless trailer
    // settles, which keeps its wheels locked like a spring brake. The native attach
    // path clears the byte when trailer_parked_state exists; manual connects must do
    // the same or the trailer drags along braked.
    constexpr uint32_t trailer_parked_state = 0x1188; // object*, non-null while the parked state is tracked
    constexpr uint32_t trailer_parked = 0x1198; // byte, 1 = parked/locked, cleared by native attach
    constexpr uint32_t trailer_parked_registered = 0x1258; // byte, 1 = parked-state record registered; gates the per-frame pose re-assertion
    constexpr uint32_t trailer_brace_state = 0x1190; // byte, 1 = landing gear down (re-asserted on parked pieces)

    // Lamp feed + light source + hose state, read out of the native lamp
    // bodies: the mask setter (0x1409DB4B0) writes the lamp byte at +0x2A0
    // (the propagator fans it down the attach tree — +0x2A1/+0x2A2 are
    // separate state, do not touch); +0x110 is the light-source context whose
    // +0x18 owner the propagator repoints to the chain head — a detached
    // trailer reads the SOURCE vehicle's mask through it, so pinning its
    // source at itself makes it read its own (zeroed) mask; +0xE8D is the
    // "lines connected" byte the cable builder checks.
    constexpr uint32_t trailer_lamp_mask = 0x02A0; // byte, bit 0 = lamp feed on
    constexpr uint32_t trailer_light_ctx = 0x0110; // light-source context object*, +0x18 = owner vehicle
    constexpr uint32_t trailer_lines_connected = 0x0E8D; // byte, 1 = hoses drawn
    constexpr uint32_t trailer_leg_up = 0x1284; // byte, 0 = landing gear down
    constexpr uint32_t trailer_gear_lever = 0x1000; // byte, 0 = gear handle in "down" position

    // More fields from the native attach/detach code: connect_slave copies the
    // coupling byte parent->slave and passes the parent's chassis-type check to
    // connect(); trailer_total_mass feeds the towing vehicle's wheel load.
    constexpr uint32_t vehicle_coupling_flag = 0x0FDB; // byte, propagated parent -> slave on connect
    constexpr uint32_t chassis_slave_hook_type = 0x0628; // dword on chassis data, == 2 marks a slave/dolly hook
    constexpr uint32_t trailer_total_mass = 0x04B8; // float, passed to the parent's wheel-load distribution

    // Trailer brake channels (1.61.1.9): the game's per-frame trailer update writes
    // trailer_brake = 1.0 and trailer_service_brake = 0 while the physics joint is
    // missing (the spring brake a detached trailer sits on), and the byte at +0x764
    // forces full brake regardless of the float. Nothing zeroes them on a manual
    // connect, so do_connect must release them itself or the wheels stay locked.
    // Verified at 0x1409fb34f / 0x140637e84 (writers) and 0x14090c562 / 0x140644181
    // (per-wheel brake consumers).
    constexpr uint32_t trailer_brake = 0x0760; // float, 0..1 brake factor applied per wheel
    constexpr uint32_t trailer_service_brake = 0x0730; // float, zeroed alongside on detach
    constexpr uint32_t trailer_brake_forced = 0x0764; // byte, != 0 forces full brake

    // Truck coupling state on the truck physics object (game_actor+0x18): the
    // per-frame trailer loop applies spring brake to every jointless trailer, and
    // to jointed ones too while this state is not 5 (fully coupled). The truck's
    // brake/electric broadcast down the towed chain is likewise gated on 5.
    // The native attach state machine ends at 5; a manual connect must write it
    // or the trailer stays braked and dark.
    // Verified at 0x1405d8c94 (spring-brake loop) and 0x14064b760 (broadcast gate).
    constexpr uint32_t truck_coupling_state = 0x14E0; // dword, 5 = trailer coupled

    // vfunc slots on the vehicle/trailer vtable: connect_slave calls [slave+0x310] on
    // the next trailer, the detach path calls [+0x318] then [+0x328](true), and the
    // re-attach path calls [+0x310] then [+0x328](false).
    constexpr uint32_t vfunc_connect = 98; // +0x310, connect(connect_to, hook_vec, a4, a5, a6)
    constexpr uint32_t vfunc_disconnect = 99; // +0x318, disconnect()
    constexpr uint32_t vfunc_set_brace = 101; // +0x328, set_trailer_brace(bool lowered)

    // Axle lift (1.61.1.9). Each trailer owns a lift controller (only created
    // when the chassis data marks at least one axle liftable). The controller's
    // init builds a per-axle bitmask at +0x38 from the axle flags; its per-frame
    // update only animates the lift progress of masked axles, and the
    // wheel-position pass raises an axle when its flag byte is set and the
    // state byte is nonzero. Writing the mask + flag bytes picks which axles
    // respond to the game's own trailer lift key. Only natively liftable axles
    // may be selected: forcing a non-liftable axle crashes the wheel/suspension
    // update, so the selection is always masked to the original flag set, and
    // nothing is written while the controller state is not "down".
    // Field is +0x6C8 in 1.61.1.9s (was +0x6A0 in 1.61) — verified via the
    // trailer init storing the controller and the set_state wrapper reading it.
    constexpr uint32_t trailer_lift_controller = 0x06C8; // axle lift controller
    constexpr uint32_t lift_controller_state = 0x003C; // byte: 0 down, 1 lowering, 2 raising, 3 up
    constexpr uint32_t lift_controller_axle_mask = 0x0038; // uint32_t, one bit per axle
    constexpr uint32_t lift_controller_axle_count = 0x0008; // uint32_t
    constexpr uint32_t lift_controller_progress = 0x0018; // float* array, per-axle lift progress
    constexpr uint32_t lift_controller_progress_count = 0x0020; // u64, same length as axles

    // The axle/chassis dyn object at trailer+0x200 (vehicle_chassis_data above)
    // holds a byte flag per axle at +0x5C0 (array_t: ptr +0x5C8, count +0x5D0).
    // The wheel-position update reads the flag plus controller state to decide
    // whether an axle is raised.
    constexpr uint32_t axle_data_flags = 0x05C8; // uint8_t* per-axle flags
    constexpr uint32_t axle_data_flags_count = 0x05D0; // u64 flag count = axle count

    // Game-actor lift bookkeeping byte: bit0 = truck axles raised, bit1 = trailer
    // axles raised. The native lift key toggles the bits so its "is lifted" checks,
    // telemetry and the toggle direction stay consistent; the plugin keeps them in
    // sync when it commands a lift itself.
    constexpr uint32_t game_actor_lift_state = 0x0248;

    template < typename T >
    T& field( void* object, const uint32_t offset )
    {
        return *reinterpret_cast< T* >( reinterpret_cast< uint8_t* >( object ) + offset );
    }
}
