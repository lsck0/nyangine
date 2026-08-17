/**
 * @file net_command.h
 *
 * What a client tells the server it is *trying* to do.
 *
 * ## A client never sends its position
 *
 * It sends intent: which actions are held, where it is aiming, which tick it believes it is on. The
 * server decides what that intent produces. This is the difference between an authoritative server
 * and a shared notepad — a client that could send its own position could send any position, and no
 * amount of validation after the fact recovers from having asked the wrong question.
 *
 * So a command carries a **bitfield of actions** rather than a delta. Which bit means what is the
 * game's business: the engine's NYA_InputAction enum reserves everything below NYA_INPUT_ACTION_USER
 * and a game continues from there, so `1 << (action - NYA_INPUT_ACTION_USER)` is the obvious mapping
 * and the engine does not need to know it.
 *
 * ## Why commands are unreliable, and sent several times
 *
 * A command is only useful for the tick it belongs to; by the time a retransmit arrived the server
 * would have simulated past it. So instead of reliability, **redundancy**: every packet carries the
 * last several ticks of commands, and the server takes whichever it has not yet seen. One lost packet
 * costs nothing at all, and it takes NYA_NET_COMMAND_REDUNDANCY consecutive losses before the server
 * has a gap to fill.
 *
 * ## What the server does with a gap
 *
 * Repeats the last command it had. A player mid-stride whose packets stop should keep walking for a
 * moment rather than stopping dead and then teleporting when they resume — and their own client is
 * predicting exactly that, so repeating is also the answer that agrees with the prediction.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetCommand NYA_NetCommand;

/**
 * How many ticks of command history ride in every packet.
 *
 * Four, because it is the point where the marginal packet loss it covers stops being worth the bytes:
 * a command is 32 bytes, so this is 128 per packet, and surviving four consecutive losses covers any
 * connection a game is playable on at all.
 * */
#define NYA_NET_COMMAND_REDUNDANCY 4

/**
 * One tick of a player's intent.
 *
 * Fixed size and flat, because it is sent every tick by every client and stored in a ring on both
 * sides. Nothing here is a pointer, so a command can be memcpy'd into a replay buffer.
 * */
struct NYA_NetCommand {
    /**
     * Which tick this describes, in the *client's* numbering.
     *
     * The server uses it to order commands and to notice duplicates; the client uses it to find the
     * command to replay after a correction. It is the client's number, so a client that lies produces
     * a worse experience for itself and no advantage — the server still applies one command per tick.
     * */
    u64 tick;

    /**
     * Which actions are held, as a game-defined bitfield.
     *
     * See the note above on mapping NYA_InputAction into it. Sixty-four bits, which is more actions
     * than any game presses at once.
     * */
    u64 actions;

    /**
     * Where the player is looking or pointing, in whatever space the game chose.
     *
     * Sent as-is rather than quantised. Aim is the one field where a small error is visible — it is
     * multiplied by the distance to whatever is being aimed at — so it keeps its full precision while
     * a position never crosses this way at all.
     * */
    f32x2 aim;

    /**
     * A game-defined scalar, for the one axis a bitfield cannot express.
     *
     * A throttle, a zoom, a charge level. Present because every game grows exactly one of these and
     * the alternative is a game event per tick.
     * */
    f32 analog;
};

/**
 * Turns a command into movement.
 *
 * Run by the server authoritatively and by the client predictively — the **same function**, which is
 * the only reason a prediction can agree with authority. It lives here rather than with either side
 * because it belongs to neither.
 *
 * Must be deterministic given `(entity state, command, delta_time_s)`. A function that reads the wall
 * clock, or an unseeded RNG, or anything the client cannot know will disagree with the server every
 * tick and the player will be corrected continuously. See net_client.h.
 * */
typedef void (*NYA_NetApplyCommandFn)(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Appends up to `count` commands, newest last.
 *
 * `count` is clamped to NYA_NET_COMMAND_REDUNDANCY. The caller passes the newest `count` entries of
 * its ring; ordering matters, because the server takes them in sequence.
 * */
NYA_API NYA_Error nya_net_command_encode(NYA_String* out, const NYA_NetCommand* commands, u32 count) __attr_no_discard;

/**
 * Reads a run of commands back.
 *
 * `out_commands` must have room for NYA_NET_COMMAND_REDUNDANCY. The count on the wire is checked
 * against that and against the payload length before anything is read — these bytes come from a peer.
 * */
NYA_API NYA_Error nya_net_command_decode(const u8* data, u64 size, OUT NYA_NetCommand* out_commands, OUT u32* out_count) __attr_no_discard;

/** Whether `action` is held in this command. `bit` is the game's own numbering. */
NYA_API b8 nya_net_command_holds(const NYA_NetCommand* command, u32 bit) __attr_no_discard;

/** Sets or clears `bit`. What a client's input sampling builds a command with. */
NYA_API void nya_net_command_set(NYA_NetCommand* command, u32 bit, b8 held);
