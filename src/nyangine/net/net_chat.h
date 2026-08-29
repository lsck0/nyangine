/**
 * @file net_chat.h
 *
 * Player chat, as a layer over NYA_NET_MSG_GAME_EVENT rather than a message kind of its own.
 *
 * A new NYA_NetMessageKind would mean bumping NYA_NET_PROTOCOL_VERSION, which refuses every older
 * client, for something the existing reliable-ordered event channel already carries. So chat is a
 * *shape of event*, identified by its `kind` field, and a build that has never heard of it ignores it
 * like any other unreadable event. It also costs nothing when unused: no socket, no tick work, no
 * state beyond a history ring that stays empty.
 *
 * On the wire — client to server, then server to every client:
 *
 * ```
 * { "kind": "chat", "text": "hello" }
 * { "kind": "chat", "text": "hello", "name": "luca", "sender": <index>, "generation": <gen>, "system": false }
 * ```
 *
 * **The client does not name itself.** It sends text and nothing else; the server stamps the name and
 * peer id from its own peer table, and nya_net_chat_server_consume never reads an incoming "name".
 * That is the whole impersonation defence, and it works because the server already knows who sent the
 * packet.
 *
 * **Chat is untrusted input twice**, so nya_net_chat_sanitize runs on both sides: on the server so a
 * hostile client cannot make everyone else render something they cannot, and on the client so a hostile
 * *server* or a bug cannot either — the server is authoritative over the world, not over what this
 * process will draw. Strict UTF-8, no control characters, no bidirectional overrides; details on that
 * function. A server-side rate limit covers the other half of the problem, how many lines arrive rather
 * than what one says. See NYA_NET_CHAT_BURST.
 *
 * No tick and nothing to initialise. It hooks the two event callbacks a game already has, each
 * returning true when it took the event:
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
 *
 * Then nya_net_chat_send to say something and nya_net_chat_count / nya_net_chat_at to draw it.
 * */
#pragma once

#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The longest a chat line may be, in **bytes** including the terminator.
 *
 * Bytes rather than characters, because that is what the buffer and the packet are measured in. A
 * line of Latin text gets most of 255 characters out of this; a line of Japanese gets around 85,
 * since those codepoints are three bytes each. That asymmetry is real and is the price of not
 * carrying a variable-length allocation for something this small.
 * */
#define NYA_NET_CHAT_TEXT_MAX 256

/**
 * How many lines are kept for display.
 *
 * A ring: the oldest is dropped when the newest arrives. Sized for a scrollback a player can read,
 * not for a log — a game that wants the whole session on disk should write it there as lines arrive
 * rather than growing this.
 * */
#define NYA_NET_CHAT_HISTORY 64

/**
 * How many lines a peer may send back to back before the limit bites.
 *
 * A token bucket rather than a minimum interval between lines, because a player typing three quick
 * replies is normal and a fixed interval punishes exactly that while barely slowing a script. The
 * bucket refills at NYA_NET_CHAT_REFILL_MS per token, so the sustained rate is one line per that
 * interval and the burst is this many.
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
     *
     * Copied rather than looked up on demand, so a line does not change its author when that player
     * disconnects and their slot is reused — which is exactly what a peer-id lookup would do.
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
 *
 * Sanitised before it goes out, so a caller may pass whatever a text field gave it. Fails if the
 * result is empty — a line of nothing but control characters is not a line — and if the client is not
 * in a game.
 *
 * Nothing is added to the local history here. The line appears when the server's broadcast comes back,
 * which is a round trip later and is the point: what you see is what everyone else saw, and if the
 * server dropped it for rate limiting you find out by it not appearing.
 * */
NYA_API NYA_Error nya_net_chat_send(NYA_ConstCString text) __attr_no_discard;

/**
 * Says something as the server itself, to everyone. Server side only.
 *
 * For "player joined", "the round is over", the answer to a command. Arrives with `is_system` set and
 * no sender, so a game can draw it differently, and is not rate limited — the server is not a peer
 * that needs restraining.
 * */
NYA_API NYA_Error nya_net_chat_broadcast_system(NYA_ConstCString text) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RECEIVING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Handles a client's event if it is a chat line. Call from NYA_NetServerEventFn.
 *
 * Validates, rate limits, stamps the sender from the server's own peer table, and rebroadcasts to
 * everyone. Returns whether the event was chat — true also when the line was *refused*, since a
 * flooding client's message is still not the game's to handle.
 *
 * Does not touch the local history. On a listen server the host is an ordinary peer and receives the
 * broadcast through nya_net_chat_client_consume like everyone else; adding it here as well would show
 * the host every line twice.
 * */
NYA_API b8 nya_net_chat_server_consume(NYA_NetPeerId peer, const NYA_Object* event);

/**
 * Handles a server event if it is a chat line, appending it to the history. Call from NYA_NetGameEventFn.
 *
 * Returns whether the event was chat. Re-sanitises: see the note on trusting the server in this file's
 * header.
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
 *
 * Marked as a system line, since that is what a local notice is from the reader's side — nobody said
 * it. Sanitised like any other, because the text usually comes from somewhere that did cross a wire.
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
 *
 * `out` is always terminated when `capacity` is at least one, so the result is a usable C string even
 * when everything was dropped — the caller checks the return value for that, not the pointer.
 *
 * What is removed, and why each one:
 *
 * - **Malformed UTF-8**, replaced with U+FFFD rather than passed along: overlong encodings, truncated
 *   sequences, stray continuation bytes, surrogate halves, anything past U+10FFFF. The result is
 *   re-encoded from decoded codepoints rather than copied, so the output is well formed whatever
 *   arrived — which matters because a text renderer's decoder handed a malformed sequence either reads
 *   past it or draws something arbitrary, and neither is a thing a remote player should get to choose.
 *   A replacement glyph is a visible, harmless "something was here".
 * - **Control characters**, C0 and C1 and DEL. Newline is one of them: a chat line is one line, and
 *   letting it contain a break lets a sender push everything else off the screen.
 * - **Bidirectional overrides and the zero-width family**, U+200B–200F, U+202A–202E, U+2066–2069,
 *   U+FEFF. These reorder the glyphs *after* them, so text can be made to display in an order the
 *   bytes do not have. That is a spoofing tool rather than a rendering bug, and chat is where it gets
 *   used.
 * - **Runs of whitespace**, collapsed to one space, and trimmed at both ends. A line of two hundred
 *   spaces is not text.
 *
 * Truncation, when the result will not fit, happens at a codepoint boundary. Cutting a multi-byte
 * sequence in half would produce exactly the malformed input this function exists to remove.
 * */
NYA_API u64 nya_net_chat_sanitize(NYA_ConstCString input, OUT char* out, u64 capacity);
