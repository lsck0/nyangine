/**
 * @file net_message.h
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/net/net_types.h"

// TYPES

typedef enum NYA_NetMessageKind NYA_NetMessageKind;

/**
 * Bumped on any change either side could misread. Checked at the handshake.
 * */
#define NYA_NET_PROTOCOL_VERSION 2

enum NYA_NetMessageKind {
    // client to server

    /**
     * "May I join, and here is who I am." Reliable, and the first thing a client ever sends.
     * */
    NYA_NET_MSG_HELLO = 1,

    /**
     * The newest snapshot applied, then what the player is trying to do for a run of recent ticks. Unreliable, every tick.
     * */
    NYA_NET_MSG_COMMAND = 2,

    // server to client

    /** "You are in." Reliable. Carries the peer's id, its entity, and the world description. */
    NYA_NET_MSG_WELCOME = 16,

    /** "You are not in, and this is why." Reliable, and the last thing sent to that peer. */
    NYA_NET_MSG_REJECT = 17,

    /** The world at a tick, delta'd against this client's acknowledged baseline. Unreliable, every tick. */
    NYA_NET_MSG_SNAPSHOT = 18,

    /** Someone joined or left, with their name. Reliable, so a player list cannot drift. */
    NYA_NET_MSG_PEER_JOINED = 19,
    NYA_NET_MSG_PEER_LEFT   = 20,

    // either direction

    /**
     * A game-defined event, as an NYA_Object. Reliable and ordered.
     * */
    NYA_NET_MSG_GAME_EVENT = 21,

    NYA_NET_MSG_COUNT,
};

// FUNCTIONS

/** Starts a message: appends the kind byte, so the caller can then append its body. */
NYA_API void nya_net_message_begin(NYA_String* out, NYA_NetMessageKind kind);

/**
 * The kind a received payload names, and where its body starts.
 * */
NYA_API NYA_NetMessageKind nya_net_message_kind(const u8* data, u64 size, OUT u64* out_body_offset) __attr_no_discard;

/**
 * Appends `object` as a serde_nya document, length-prefixed.
 * */
NYA_API NYA_Error nya_net_message_write_object(NYA_Arena* arena, NYA_String* out, const NYA_Object* object) __attr_no_discard;

/**
 * Reads a length-prefixed serde_nya document back.
 * */
NYA_API NYA_Error nya_net_message_read_object(NYA_Arena* arena, const u8* data, u64 size, OUT NYA_Object** out_object) __attr_no_discard;
