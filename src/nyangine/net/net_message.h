/**
 * @file net_message.h
 *
 * What the server and client actually say to each other, on top of the transport's "here are some
 * bytes".
 *
 * Every message begins with one byte naming its kind. That byte is the only thing the framing
 * imposes; what follows is whatever the kind's own encoding says, and the two encodings in use are
 * deliberately different:
 *
 * - **Snapshots and commands** are flat records — see net_snapshot.h. They go out every tick, per
 *   client, so their cost is multiplied by everything and they are shaped for that.
 * - **Everything structural** is a serde_nya document: the handshake, the world description, a chat
 *   line, a player list. These happen once, or once per event, and a self-describing format that can
 *   gain a field without breaking an older peer is worth its bytes there.
 *
 * That split is the whole reason net_snapshot.c does not use serde_nya, and the reason this file does.
 *
 * ## Versioning
 *
 * NYA_NET_PROTOCOL_VERSION is checked in the handshake, before any state is exchanged, and a mismatch
 * refuses the connection outright. That is deliberately strict: a peer that misparses a snapshot does
 * not crash, it plays a subtly different game, and there is no way for either side to notice. Better
 * to refuse a connection a player can understand than to allow one nobody can debug.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_NetMessageKind NYA_NetMessageKind;

/**
 * Bumped on any change either side could misread. Checked at the handshake.
 *
 * Separate from NYA_NET_SNAPSHOT_VERSION because they change for different reasons and a peer may
 * legitimately want to know which of the two disagrees — though today a mismatch in either refuses
 * the connection the same way.
 * */
#define NYA_NET_PROTOCOL_VERSION 1

enum NYA_NetMessageKind {
    /*
     * ── client to server ──
     */

    /**
     * "May I join, and here is who I am." Reliable, and the first thing a client ever sends.
     *
     * Carries the protocol and snapshot versions, and the player's name. Answered with WELCOME or
     * REJECT; nothing else is accepted from a peer until this has been.
     * */
    NYA_NET_MSG_HELLO = 1,

    /**
     * What the player is trying to do, for a run of recent ticks. Unreliable, every tick.
     *
     * A run rather than one, because the channel drops packets: each carries the last several ticks
     * of input, so a lost packet costs nothing and the server has the gap filled by the next.
     * */
    NYA_NET_MSG_COMMAND = 2,

    /**
     * "I have applied the snapshot for tick N." Unreliable, ridden along with commands.
     *
     * What lets the server delta against something this client has definitely seen. Losing one costs
     * nothing: the next names a tick at least as high.
     * */
    NYA_NET_MSG_SNAPSHOT_ACK = 3,

    /*
     * ── server to client ──
     */

    /** "You are in." Reliable. Carries the peer's id, its entity, and the world description. */
    NYA_NET_MSG_WELCOME = 16,

    /** "You are not in, and this is why." Reliable, and the last thing sent to that peer. */
    NYA_NET_MSG_REJECT = 17,

    /** The world at a tick, delta'd against this client's acknowledged baseline. Unreliable, every tick. */
    NYA_NET_MSG_SNAPSHOT = 18,

    /** Someone joined or left, with their name. Reliable, so a player list cannot drift. */
    NYA_NET_MSG_PEER_JOINED = 19,
    NYA_NET_MSG_PEER_LEFT   = 20,

    /*
     * ── either direction ──
     */

    /**
     * A game-defined event, as an NYA_Object. Reliable and ordered.
     *
     * The escape hatch, and the only message whose contents the engine does not interpret: a chat
     * line, an inventory change, "the boss died". A game that needs to say something the engine has
     * no opinion about says it here rather than by extending this enum.
     * */
    NYA_NET_MSG_GAME_EVENT = 21,

    NYA_NET_MSG_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Starts a message: appends the kind byte, so the caller can then append its body. */
NYA_API void nya_net_message_begin(NYA_String* out, NYA_NetMessageKind kind);

/**
 * The kind a received payload names, and where its body starts.
 *
 * NYA_NET_MSG_COUNT for an empty payload or one naming a kind this build does not know — which is
 * not necessarily an attack. A newer peer may send a kind this one has never heard of, and the right
 * answer is to ignore that message rather than to drop the connection.
 * */
NYA_API NYA_NetMessageKind nya_net_message_kind(const u8* data, u64 size, OUT u64* out_body_offset) __attr_no_discard;

/**
 * Appends `object` as a serde_nya document, length-prefixed.
 *
 * The length prefix is what lets a structural message sit in the same payload as anything else, and
 * what lets a reader skip a document it does not understand rather than guessing where it ended.
 * */
NYA_API NYA_Error nya_net_message_write_object(NYA_Arena* arena, NYA_String* out, const NYA_Object* object) __attr_no_discard;

/**
 * Reads a length-prefixed serde_nya document back.
 *
 * Bounds-checked against `size` rather than trusting the prefix, because the prefix came from a peer.
 * NYA_ERROR_INVALID_ARGUMENT on anything malformed.
 * */
NYA_API NYA_Error nya_net_message_read_object(NYA_Arena* arena, const u8* data, u64 size, OUT NYA_Object** out_object) __attr_no_discard;
