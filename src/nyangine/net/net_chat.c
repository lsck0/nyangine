#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The `kind` every chat event carries. See the wire shape in net_chat.h. */
#define _NYA_NET_CHAT_KIND "chat"

/**
 * How far into an oversized line the sanitiser will read before giving up, in bytes.
 *
 * The output is capped at NYA_NET_CHAT_TEXT_MAX, so a long line already stops costing anything once
 * the buffer fills — except when what it holds is *droppable*. A megabyte of spaces, or of bidi
 * overrides, never fills the output and would be walked to the end. Eight times the output bounds
 * that at a size no real line comes close to.
 * */
#define _NYA_NET_CHAT_SCAN_MAX (NYA_NET_CHAT_TEXT_MAX * 8)

typedef struct {
    /** Whose budget this is. A slot is reused when a peer disconnects, so the id is checked, not just the index. */
    NYA_NetPeerId peer;

    /** Fractional, so the refill does not have to land on a whole line to count. */
    f32 tokens;

    u64 last_ms;
} _NYA_NetChatBucket;

typedef struct {
    NYA_NetChatMessage entries[NYA_NET_CHAT_HISTORY];

    /** How many of `entries` hold a line, saturating at NYA_NET_CHAT_HISTORY. */
    u32 count;

    /** Where the oldest line is once the ring has wrapped. Zero until then. */
    u32 head;

    _NYA_NetChatBucket buckets[NYA_NET_MAX_PEERS];
} _NYA_NetChatState;

NYA_INTERNAL _NYA_NetChatState _NYA_NET_CHAT = { 0 };

NYA_INTERNAL b8                  _nya_net_chat_is_chat(const NYA_Object* event);
NYA_INTERNAL b8                  _nya_net_chat_is_removed(u32 codepoint);
NYA_INTERNAL b8                  _nya_net_chat_is_space(u32 codepoint);
NYA_INTERNAL u32                 _nya_net_chat_encode(u32 codepoint, OUT char* out);
NYA_INTERNAL b8                  _nya_net_chat_allow(NYA_NetPeerId peer);
NYA_INTERNAL NYA_NetChatMessage* _nya_net_chat_push(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SANITISING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether a codepoint is stripped rather than displayed.
 *
 * Each range is here for a reason and none of them is "it looked odd"; see the list in net_chat.h.
 * Everything else is let through deliberately, including scripts this build has no font for — a
 * missing glyph is the font's problem and shows as a box, which is a far better failure than a
 * filter that decides which languages are allowed.
 * */
NYA_INTERNAL b8 _nya_net_chat_is_removed(u32 codepoint) {
    // C0 and DEL. Newline and tab are in here, which is intended: a chat line is one line.
    if (codepoint < 0x20 || codepoint == 0x7F) return true;

    // C1. Reachable in UTF-8 even though a terminal would need an escape for them.
    if (codepoint >= 0x80 && codepoint <= 0x9F) return true;

    // Zero width space and joiners, and the LTR/RTL marks.
    if (codepoint >= 0x200B && codepoint <= 0x200F) return true;

    // Bidirectional embeddings and overrides: these reorder the glyphs that follow.
    if (codepoint >= 0x202A && codepoint <= 0x202E) return true;

    // The bidirectional isolates, which are the newer spelling of the same capability.
    if (codepoint >= 0x2066 && codepoint <= 0x2069) return true;

    // Zero width no-break space, better known as a byte order mark in the middle of a string.
    if (codepoint == 0xFEFF) return true;

    return false;
}

/**
 * Whether a codepoint collapses into a single space.
 *
 * Only the space characters that survive _nya_net_chat_is_removed, so tab and newline are absent —
 * they are already gone. U+00A0 and U+3000 are here because they *render* as blank width, and a line
 * padded with a hundred of them is the same abuse as one padded with spaces.
 * */
NYA_INTERNAL b8 _nya_net_chat_is_space(u32 codepoint) {
    return codepoint == 0x20 || codepoint == 0x00A0 || codepoint == 0x3000;
}

/**
 * Writes one codepoint as UTF-8 into `out`, which must hold four bytes, and answers how many it used.
 *
 * The counterpart to nya_utf8_next, and the reason sanitising *re-encodes* rather than copying the
 * input's bytes: whatever arrives, what leaves here is well formed by construction. A malformed
 * sequence decodes to U+FFFD and is written as the three bytes of U+FFFD, so it displays as one
 * replacement glyph instead of being passed along for the text renderer's decoder to trip over.
 * */
NYA_INTERNAL u32 _nya_net_chat_encode(u32 codepoint, OUT char* out) {
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }

    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }

    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    // Four bytes, and no five byte case: nya_utf8_next never yields anything above U+10FFFF.
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

u64 nya_net_chat_sanitize(NYA_ConstCString input, OUT char* out, u64 capacity) {
    if (out == nullptr || capacity == 0) return 0;

    // Terminated up front, so every early return below leaves a usable empty string rather than
    // whatever the caller's buffer happened to hold.
    out[0] = '\0';

    if (input == nullptr) return 0;

    u64 written = 0;
    u64 scanned = 0;

    /*
     * A space is held back until something printable follows it.
     *
     * That one flag does all three whitespace jobs at once: a leading run never commits because
     * nothing has been written yet, an interior run commits exactly once, and a trailing run is still
     * pending when the loop ends and is simply never written. The alternative is a trim pass over the
     * result, which would have to be a second walk.
     */
    b8 pending_space = false;

    for (NYA_ConstCString cursor = input; *cursor != '\0';) {
        if (scanned >= _NYA_NET_CHAT_SCAN_MAX) break;

        u32 codepoint = 0;
        u32 used      = nya_utf8_next(cursor, &codepoint);

        cursor  += used;
        scanned += used;

        if (_nya_net_chat_is_removed(codepoint)) continue;

        if (_nya_net_chat_is_space(codepoint)) {
            pending_space = written > 0;
            continue;
        }

        char encoded[4] = { 0 };
        u32  size       = _nya_net_chat_encode(codepoint, encoded);

        u64 needed = (u64)size + (pending_space ? 1 : 0);

        /*
         * The line ends here rather than being cut to fit.
         *
         * `+ 1` leaves room for the terminator. Stopping on the whole codepoint is the point: writing
         * as many of its bytes as fit would leave a truncated sequence in the output, which is exactly
         * the malformed input this function exists to remove.
         */
        if (written + needed + 1 > capacity) break;

        if (pending_space) {
            out[written] = ' ';
            written++;
            pending_space = false;
        }

        for (u32 i = 0; i < size; i++) {
            out[written] = encoded[i];
            written++;
        }
    }

    out[written] = '\0';
    return written;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HISTORY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The slot the next line goes in, cleared and stamped. Drops the oldest once the ring is full. */
NYA_INTERNAL NYA_NetChatMessage* _nya_net_chat_push(void) {
    NYA_NetChatMessage* slot = nullptr;

    if (_NYA_NET_CHAT.count < NYA_NET_CHAT_HISTORY) {
        slot = &_NYA_NET_CHAT.entries[_NYA_NET_CHAT.count];
        _NYA_NET_CHAT.count++;
    } else {
        // Full: the oldest line is where the new one goes, and the window slides forward.
        slot = &_NYA_NET_CHAT.entries[_NYA_NET_CHAT.head];

        _NYA_NET_CHAT.head = (_NYA_NET_CHAT.head + 1) % NYA_NET_CHAT_HISTORY;
    }

    *slot = (NYA_NetChatMessage){ 0 };

    slot->received_ms = nya_clock_get_monotonic_ms();

    return slot;
}

u32 nya_net_chat_count(void) {
    return _NYA_NET_CHAT.count;
}

const NYA_NetChatMessage* nya_net_chat_at(u32 index) {
    if (index >= _NYA_NET_CHAT.count) return nullptr;

    // `head` is zero until the ring wraps, so this is the identity in the common case.
    return &_NYA_NET_CHAT.entries[(_NYA_NET_CHAT.head + index) % NYA_NET_CHAT_HISTORY];
}

void nya_net_chat_append_local(NYA_ConstCString text) {
    char clean[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    // Sanitised even though it never touched a socket. A local notice is often built from something
    // that *did* — an error string, a name — and the renderer's guarantee should not depend on which.
    if (nya_net_chat_sanitize(text, clean, sizeof(clean)) == 0) return;

    NYA_NetChatMessage* message = _nya_net_chat_push();

    message->sender    = NYA_NET_PEER_NONE;
    message->is_system = true;

    (void)snprintf(message->text, sizeof(message->text), "%s", clean);
}

void nya_net_chat_clear(void) {
    _NYA_NET_CHAT.count = 0;
    _NYA_NET_CHAT.head  = 0;

    // The buckets go too. They are keyed by peer id and a new session hands out the same indices
    // again, so a stale budget would follow a slot number into a game it has nothing to do with.
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) _NYA_NET_CHAT.buckets[i] = (_NYA_NetChatBucket){ 0 };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RATE LIMIT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `peer` may say one more line now, spending a token if so. See NYA_NET_CHAT_BURST. */
NYA_INTERNAL b8 _nya_net_chat_allow(NYA_NetPeerId peer) {
    if (peer.index >= NYA_NET_MAX_PEERS) return false;

    _NYA_NetChatBucket* bucket = &_NYA_NET_CHAT.buckets[peer.index];

    u64 now_ms = nya_clock_get_monotonic_ms();

    /*
     * A slot occupied by a different connection than last time starts full.
     *
     * Peer slots are reused, so without this a reconnecting player either inherits the previous
     * occupant's exhausted budget or, worse, a flooder gets a fresh one by reconnecting. Keying on the
     * whole id — index *and* generation — makes both impossible.
     */
    if (!nya_net_peer_equals(bucket->peer, peer)) {
        *bucket = (_NYA_NetChatBucket){ .peer = peer, .tokens = (f32)NYA_NET_CHAT_BURST, .last_ms = now_ms };
    }

    // Monotonic, so this cannot wrap and the subtraction needs no guard.
    u64 elapsed_ms = now_ms - bucket->last_ms;

    bucket->last_ms = now_ms;

    bucket->tokens += (f32)elapsed_ms / (f32)NYA_NET_CHAT_REFILL_MS;

    if (bucket->tokens > (f32)NYA_NET_CHAT_BURST) bucket->tokens = (f32)NYA_NET_CHAT_BURST;

    if (bucket->tokens < 1.0F) return false;

    bucket->tokens -= 1.0F;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * EVENTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether this event is a chat line at all. Anything else belongs to the game and is left alone. */
NYA_INTERNAL b8 _nya_net_chat_is_chat(const NYA_Object* event) {
    if (event == nullptr) return false;

    NYA_Value* kind = nya_object_get(event, "kind");

    if (kind == nullptr || kind->type != NYA_TYPE_STRING || kind->as_string == nullptr) return false;

    return nya_string_equals((NYA_ConstCString)kind->as_string, _NYA_NET_CHAT_KIND);
}

NYA_Error nya_net_chat_send(NYA_ConstCString text) {
    char clean[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    if (nya_net_chat_sanitize(text, clean, sizeof(clean)) == 0) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nothing left to say after sanitising");
    }

    NYA_Arena* scratch = nya_arena_create(.name = "net_chat_send");
    defer      nya_arena_destroy(scratch);

    NYA_Object* event = nya_object_create(scratch);

    nya_object_set(event, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_NYA_NET_CHAT_KIND });
    nya_object_set(event, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = clean });

    // No name and no id. The server fills both in from its own table; see net_chat.h on why sending
    // them would be pointless rather than dangerous.
    return nya_net_client_send_event(event);
}

NYA_Error nya_net_chat_broadcast_system(NYA_ConstCString text) {
    char clean[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    if (nya_net_chat_sanitize(text, clean, sizeof(clean)) == 0) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nothing left to say after sanitising");
    }

    NYA_Arena* scratch = nya_arena_create(.name = "net_chat_system");
    defer      nya_arena_destroy(scratch);

    NYA_Object* event = nya_object_create(scratch);

    nya_object_set(event, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_NYA_NET_CHAT_KIND });
    nya_object_set(event, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = clean });
    nya_object_set(event, "system", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    return nya_net_server_send_event(NYA_NET_PEER_NONE, event);
}

b8 nya_net_chat_server_consume(NYA_NetPeerId peer, const NYA_Object* event) {
    if (!_nya_net_chat_is_chat(event)) return false;

    /*
     * Refused *before* the text is looked at.
     *
     * Deliberate ordering: sanitising is the expensive part of handling a line, so a peer that floods
     * should not get that work done on its behalf. The cost of a refused line is a bucket lookup.
     */
    if (!_nya_net_chat_allow(peer)) return true;

    NYA_Value* text = nya_object_get(event, "text");

    if (text == nullptr || text->type != NYA_TYPE_STRING || text->as_string == nullptr) return true;

    char clean[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    if (nya_net_chat_sanitize((NYA_ConstCString)text->as_string, clean, sizeof(clean)) == 0) return true;

    /*
     * The sender's name comes from the server's peer table, never from the message.
     *
     * This is the whole impersonation defence and it is one lookup: the server already knows which
     * connection the packet arrived on, so a "name" field in the event has nothing to add and is not
     * read. A peer that has not completed the handshake has no entry and is dropped here.
     */
    const NYA_NetServerPeer* sender = nya_net_server_peer(peer);

    if (sender == nullptr || !sender->accepted) return true;

    NYA_Arena* scratch = nya_arena_create(.name = "net_chat_relay");
    defer      nya_arena_destroy(scratch);

    NYA_Object* out = nya_object_create(scratch);

    nya_object_set(out, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_NYA_NET_CHAT_KIND });
    nya_object_set(out, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = clean });
    nya_object_set(out, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)sender->name });
    nya_object_set(out, "sender", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = peer.index });
    nya_object_set(out, "generation", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = peer.generation });

    NYA_Error sent = nya_net_server_send_event(NYA_NET_PEER_NONE, out);

    // Logged rather than propagated: the caller is an event callback with nowhere to return an error
    // to, and a dedicated server wants the line in its output anyway.
    if (!sent.ok) {
        nya_log_warn("Could not relay a chat line from %s: %s", sender->name, sent.message);
    } else {
        nya_log_info("[chat] %s: %s", sender->name, clean);
    }

    return true;
}

b8 nya_net_chat_client_consume(const NYA_Object* event) {
    if (!_nya_net_chat_is_chat(event)) return false;

    NYA_Value* text = nya_object_get(event, "text");

    if (text == nullptr || text->type != NYA_TYPE_STRING || text->as_string == nullptr) return true;

    char clean[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    // Sanitised again on arrival. The server is authoritative over the world, not over what this
    // process will draw; see the header. On a listen server this is the same call twice on the same
    // bytes, which is cheap and keeps the remote path and the local one identical.
    if (nya_net_chat_sanitize((NYA_ConstCString)text->as_string, clean, sizeof(clean)) == 0) return true;

    NYA_Value* name       = nya_object_get(event, "name");
    NYA_Value* sender     = nya_object_get(event, "sender");
    NYA_Value* generation = nya_object_get(event, "generation");
    NYA_Value* system     = nya_object_get(event, "system");

    NYA_NetChatMessage* message = _nya_net_chat_push();

    (void)snprintf(message->text, sizeof(message->text), "%s", clean);

    message->is_system = system != nullptr && system->type == NYA_TYPE_B8 && system->as_b8;

    if (message->is_system) return true;

    message->sender.index      = sender != nullptr && sender->type == NYA_TYPE_U32 ? sender->as_u32 : 0;
    message->sender.generation = generation != nullptr && generation->type == NYA_TYPE_U32 ? generation->as_u32 : 0;

    /*
     * The name is sanitised too, into its own smaller buffer.
     *
     * A name reaches this process through the same untrusted path a line does — the server copied it
     * from a HELLO — and it is drawn next to the line. Nothing else validates it on the way, so this
     * is where it happens.
     */
    if (name != nullptr && name->type == NYA_TYPE_STRING && name->as_string != nullptr) {
        (void)nya_net_chat_sanitize((NYA_ConstCString)name->as_string, message->name, sizeof(message->name));
    }

    if (message->name[0] == '\0') (void)snprintf(message->name, sizeof(message->name), "player");

    return true;
}
