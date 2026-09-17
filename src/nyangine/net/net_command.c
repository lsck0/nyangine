#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ENCODING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A command run is a count byte, then each command oldest first:
 *
 * ```
 * presence u8     bit 0: aim follows, bit 1: analog follows. Absent means the same as the command before.
 * tick            varint: the first absolute, the rest as the step from the one before, at least one
 * actions         varint
 * aim             f32, f32
 * analog          f32
 * ```
 *
 * A player holding still sends the same aim run after run, so a steady run is a few bytes a command.
 */

#define _NYA_NET_COMMAND_AIM    (1U << 0)
#define _NYA_NET_COMMAND_ANALOG (1U << 1)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_command_encode(NYA_String* out, const NYA_NetCommand* commands, u32 count) {
    nya_assert(out != nullptr);

    if (commands == nullptr || count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no commands to encode");

    // clamped rather than refused: a caller handing over its whole ring is asking for "as many as fit".
    if (count > NYA_NET_COMMAND_REDUNDANCY) count = NYA_NET_COMMAND_REDUNDANCY;

    for (u32 i = 1; i < count; i++) {
        if (commands[i].tick <= commands[i - 1].tick) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run out of tick order");
    }

    nya_string_push_back(out, (u8)count);

    for (u32 i = 0; i < count; i++) {
        const NYA_NetCommand* command  = &commands[i];
        const NYA_NetCommand* previous = i == 0 ? nullptr : &commands[i - 1];

        b8 aim    = previous == nullptr || previous->aim.x != command->aim.x || previous->aim.y != command->aim.y;
        b8 analog = previous == nullptr || previous->analog != command->analog;

        nya_string_push_back(out, (u8)((aim ? _NYA_NET_COMMAND_AIM : 0U) | (analog ? _NYA_NET_COMMAND_ANALOG : 0U)));

        _nya_net_write_varint(out, previous == nullptr ? command->tick : command->tick - previous->tick);
        _nya_net_write_varint(out, command->actions);

        if (aim) {
            _nya_net_write_f32(out, command->aim.x);
            _nya_net_write_f32(out, command->aim.y);
        }

        if (analog) _nya_net_write_f32(out, command->analog);
    }

    return NYA_OK;
}

NYA_Error nya_net_command_decode(const u8* data, u64 size, OUT NYA_NetCommand* out_commands, OUT u32* out_count) {
    nya_assert(out_commands != nullptr);
    nya_assert(out_count != nullptr);

    *out_count = 0;

    if (data == nullptr || size < 1) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty command payload");

    u32 count = data[0];

    // checked before a single command is written: the caller's array holds exactly the limit.
    if (count > NYA_NET_COMMAND_REDUNDANCY) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run of %u, past the %d limit", count, NYA_NET_COMMAND_REDUNDANCY);
    }

    _NYA_NetReader reader = { .data = data, .size = size, .at = 1 };

    NYA_NetCommand previous = { 0 };

    for (u32 i = 0; i < count; i++) {
        u8  presence = _nya_net_read_u8(&reader);
        u64 step     = _nya_net_read_varint(&reader);

        NYA_NetCommand command = previous;

        command.actions = _nya_net_read_varint(&reader);

        if (presence & _NYA_NET_COMMAND_AIM) {
            // into named locals first, since the order a compound literal's initialisers run in is unspecified.
            f32 aim_x = _nya_net_read_f32(&reader);
            f32 aim_y = _nya_net_read_f32(&reader);

            command.aim = (f32x2){ aim_x, aim_y };
        }

        if (presence & _NYA_NET_COMMAND_ANALOG) command.analog = _nya_net_read_f32(&reader);

        if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run truncated at %u", i);

        // the first command has to say what it is, and the rest only move forward, without wrapping.
        if ((presence & ~(u8)(_NYA_NET_COMMAND_AIM | _NYA_NET_COMMAND_ANALOG)) != 0 || (i == 0 && presence != (_NYA_NET_COMMAND_AIM | _NYA_NET_COMMAND_ANALOG))) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command with presence bits %u", presence);
        }

        if (i > 0 && (step == 0 || step > U64_MAX - previous.tick)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run out of tick order");

        // a value no input device produces, and one that would poison whatever the game does with it.
        if (!isfinite(command.aim.x) || !isfinite(command.aim.y) || !isfinite(command.analog)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command with a non-finite aim or analog");
        }

        command.tick = i == 0 ? step : previous.tick + step;

        out_commands[i] = command;
        previous        = command;
    }

    if (reader.at != size) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command run with %llu bytes past its end", (unsigned long long)(size - reader.at));

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
        nya_log_warn("Action bit %u is past the 64 a command carries; it will never be sent.", bit);
        return;
    }

    if (held) command->actions |= 1ULL << bit;
    else command->actions &= ~(1ULL << bit);
}
