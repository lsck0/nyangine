/**
 * @file replicate_command.h
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_entity.h"
#include "nyangine-std/math/math_vector.h"
#include "nyangine-core/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetCommand NYA_NetCommand;

/**
 * How many ticks of command history ride in every packet.
 * */
#define NYA_NET_COMMAND_REDUNDANCY 4

/**
 * One tick of a player's intent.
 * */
struct NYA_NetCommand {
    /**
     * Which tick this describes, in the *client's* numbering.
     * */
    u64 tick;

    /**
     * Which actions are held, as a game-defined bitfield.
     * */
    u64 actions;

    /**
     * Where the player is looking or pointing, in whatever space the game chose.
     * */
    f32x2 aim;

    /**
     * A game-defined scalar, for the one axis a bitfield cannot express.
     * */
    f32 analog;
};

/**
 * Turns a command into movement.
 * */
typedef void (*NYA_NetApplyCommandFn)(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Appends up to `count` commands, newest last.
 * */
NYA_API NYA_Error nya_net_command_encode(NYA_String* out, const NYA_NetCommand* commands, u32 count) __attr_no_discard;

/**
 * Reads a run of commands back.
 * */
NYA_API NYA_Error nya_net_command_decode(const u8* data, u64 size, OUT NYA_NetCommand* out_commands, OUT u32* out_count) __attr_no_discard;

/** Whether `action` is held in this command. `bit` is the game's own numbering. */
NYA_API b8 nya_net_command_holds(const NYA_NetCommand* command, u32 bit) __attr_no_discard;

/** Sets or clears `bit`. What a client's input sampling builds a command with. */
NYA_API void nya_net_command_set(NYA_NetCommand* command, u32 bit, b8 held);
