#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ENCODING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A command run is:
 *
 *     u8 count
 *     then, per command, oldest first:
 *         u64 tick
 *         u64 actions
 *         f32 aim_x
 *         f32 aim_y
 *         f32 analog
 *
 * Twenty-eight bytes per command. Not delta'd against the previous one: at four commands per packet
 * the header for a delta mask would cost more than the bytes it saved, and a command is mostly a
 * bitfield that either changed or did not.
 *
 * Ticks are absolute rather than relative to the newest. A relative encoding would be two bytes
 * shorter and would make a packet uninterpretable on its own, which matters here precisely because
 * these packets are the ones expected to arrive out of order.
 */

#define _NYA_NET_COMMAND_SIZE 28

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_command_encode(NYA_String* out, const NYA_NetCommand* commands, u32 count) {
    nya_assert(out != nullptr);

    if (commands == nullptr || count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no commands to encode");

    // Clamped rather than refused: a caller handing over its whole ring is asking for "as many as fit",
    // and the redundancy limit is the transport's business rather than the caller's to remember.
    if (count > NYA_NET_COMMAND_REDUNDANCY) count = NYA_NET_COMMAND_REDUNDANCY;

    nya_string_push_back(out, (u8)count);

    for (u32 i = 0; i < count; i++) {
        const NYA_NetCommand* command = &commands[i];

        _nya_net_write_u64(out, command->tick);
        _nya_net_write_u64(out, command->actions);
        _nya_net_write_f32(out, command->aim.x);
        _nya_net_write_f32(out, command->aim.y);
        _nya_net_write_f32(out, command->analog);
    }

    return NYA_OK;
}

NYA_Error nya_net_command_decode(const u8* data, u64 size, OUT NYA_NetCommand* out_commands, OUT u32* out_count) {
    nya_assert(out_commands != nullptr);
    nya_assert(out_count != nullptr);

    *out_count = 0;

    if (data == nullptr || size < 1) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty command payload");

    u32 count = data[0];

    /*
     * Both bounds checked before a single command is read.
     *
     * `count` is a byte a peer chose, and `out_commands` holds exactly NYA_NET_COMMAND_REDUNDANCY —
     * so a peer claiming 255 commands would write past the caller's array. That array is on the
     * server's stack, one call away from a socket.
     */
    if (count > NYA_NET_COMMAND_REDUNDANCY) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run of %u, past the %d limit", count, NYA_NET_COMMAND_REDUNDANCY);
    }

    if (size < 1 + ((u64)count * _NYA_NET_COMMAND_SIZE)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run of %u in %llu bytes", count, (unsigned long long)size);
    }

    _NYA_NetReader reader = { .data = data, .size = size, .at = 1 };

    for (u32 i = 0; i < count; i++) {
        NYA_NetCommand command = { 0 };

        command.tick    = _nya_net_read_u64(&reader);
        command.actions = _nya_net_read_u64(&reader);

        // Into named locals first: the order in which a compound literal's initialisers are evaluated
        // is unspecified, and this has to consume the stream left to right.
        f32 aim_x = _nya_net_read_f32(&reader);
        f32 aim_y = _nya_net_read_f32(&reader);

        command.aim    = (f32x2){ aim_x, aim_y };
        command.analog = _nya_net_read_f32(&reader);

        if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run truncated at %u", i);

        out_commands[i] = command;
    }

    *out_count = count;

    return NYA_OK;
}

b8 nya_net_command_holds(const NYA_NetCommand* command, u32 bit) {
    nya_assert(command != nullptr);

    // Bounded, because the bit comes from a game's own action numbering and a shift past the width of
    // the type is undefined rather than merely zero.
    if (bit >= 64) return false;

    return (command->actions & (1ULL << bit)) != 0;
}

void nya_net_command_set(NYA_NetCommand* command, u32 bit, b8 held) {
    nya_assert(command != nullptr);

    if (bit >= 64) {
        // Reported rather than ignored: a game that has run out of action bits has a real problem and
        // silently dropping the highest ones would look like an input bug.
        nya_warn("Action bit %u is past the 64 a command carries; it will never be sent.", bit);
        return;
    }

    if (held) command->actions |= 1ULL << bit;
    else command->actions &= ~(1ULL << bit);
}
