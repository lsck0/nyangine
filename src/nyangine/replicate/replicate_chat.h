/**
 * @file replicate_chat.h
 *
 * ```
 * { "kind": "chat", "text": "hello" }
 * { "kind": "chat", "text": "hello", "name": "luca", "sender": <index>, "generation": <gen>, "system": false }
 * ```
 *
 * ```c
 * void on_client_event(NYA_NetPeerId peer, const NYA_Object* event) {
 *     if (nya_net_chat_server_consume(peer, event)) return;
 *     // ... the game's own events
 * }
 *
 * void on_game_event(const NYA_Object* event) {
 *     if (nya_net_chat_client_consume(event)) return;
 *     // ... the game's own events
 * }
 * ```
 * */
#pragma once

#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The longest a chat line may be, in bytes including the terminator. */
#define NYA_NET_CHAT_TEXT_MAX 256

/**
 * How many lines are kept for display.
 * */
#define NYA_NET_CHAT_HISTORY 64

/**
 * How many lines a peer may send back to back before the limit bites.
 * */
#define NYA_NET_CHAT_BURST 5

/** How long one token takes to come back. See NYA_NET_CHAT_BURST. */
#define NYA_NET_CHAT_REFILL_MS 1500

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetChatMessage NYA_NetChatMessage;

/**
 * One line, as it is kept for display. Already sanitised: safe to hand straight to the text renderer.
 *
 * @reflect
 * */
struct NYA_NetChatMessage {
    /** Who said it. NYA_NET_PEER_NONE for a system line, which nobody said. */
    NYA_NetPeerId sender;

    /**
     * Their name at the moment they said it.
     * */
    char name[NYA_NET_MAX_NAME];

    char text[NYA_NET_CHAT_TEXT_MAX];

    /** When this process received it, from nya_clock_get_monotonic_ms. For fading old lines out. */
    u64 received_ms;

    /** The server talking rather than a player. See nya_net_chat_broadcast_system. */
    b8 is_system;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SENDING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Says something, as this client's player.
 * */
NYA_API NYA_Error nya_net_chat_send(NYA_ConstCString text) __attr_no_discard;

/**
 * Says something as the server itself, to everyone. Server side only.
 * */
NYA_API NYA_Error nya_net_chat_broadcast_system(NYA_ConstCString text) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RECEIVING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Handles a client's event if it is a chat line. Call from NYA_NetServerEventFn.
 * */
NYA_API b8 nya_net_chat_server_consume(NYA_NetPeerId peer, const NYA_Object* event);

/**
 * Handles a server event if it is a chat line, appending it to the history. Call from NYA_NetGameEventFn.
 * */
NYA_API b8 nya_net_chat_client_consume(const NYA_Object* event);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HISTORY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many lines are held, at most NYA_NET_CHAT_HISTORY. */
NYA_API u32 nya_net_chat_count(void) __attr_no_discard;

/** Line `index`, oldest first, or null past the end. The pointer is valid until the next line arrives. */
NYA_API const NYA_NetChatMessage* nya_net_chat_at(u32 index) __attr_no_discard;

/**
 * Appends a line locally, without sending anything. For notices nobody else should see.
 * */
NYA_API void nya_net_chat_append_local(NYA_ConstCString text);

/** Forgets every line. What disconnecting should do, so the next game does not open on the last one's chat. */
NYA_API void nya_net_chat_clear(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SANITISING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Copies `input` into `out` with everything unsafe to display removed. Returns the bytes written.
 * */
NYA_API u64 nya_net_chat_sanitize(NYA_ConstCString input, OUT char* out, u64 capacity);
