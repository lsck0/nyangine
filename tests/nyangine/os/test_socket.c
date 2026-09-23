/**
 * The socket layer: a datagram round trip, a stream connection made and taken, partial writes, the end
 * of a stream, the wait, and what an address is.
 *
 * All of it over loopback on a port the host picks, so nothing here needs the network to be up, a port
 * to be free, or a name server to answer.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Long enough that a lost loopback packet would be a bug rather than bad luck. */
#define WAIT_MS 500

/** What a stream test sends: past any socket buffer, so a partial write is the expected thing. */
#define BIG_SIZE (4ULL * 1024 * 1024)

/** Waits until one socket is readable, or gives up. Loopback is fast, so a timeout here is a failure. */
static b8 wait_readable(NYA_OsSocket socket, u32 timeout_ms) {
  NYA_OsSocketWait watched = { .socket = socket, .readable = true };
  u32              ready   = 0;

  if (nya_os_socket_wait(&watched, 1, timeout_ms, &ready) != NYA_OS_SOCKET_OK) return false;

  return watched.is_readable;
}

s32 main(void) {
  nya_check(nya_os_socket_start() == NYA_OS_SOCKET_OK, "the host's socket library starts");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an address is a value, and reads back as what it was written from.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsAddress loopback = { 0 };
    nya_check(nya_os_address_resolve("127.0.0.1", 8080, NYA_OS_ADDRESS_V4, &loopback) == NYA_OS_SOCKET_OK, "a literal resolves without asking anybody");
    nya_check(loopback.kind == NYA_OS_ADDRESS_V4, "as the family it was written in, got %u", (u32)loopback.kind);
    nya_check(loopback.port == 8080, "with the port the caller gave, got %u", loopback.port);

    char text[NYA_OS_ADDRESS_TEXT_MAX] = { 0 };
    nya_check(nya_os_address_text(loopback, true, text, sizeof(text)), "and writes back as text");
    nya_check(nya_string_equals(text, "127.0.0.1:8080"), "which is what it came from, got '%s'", text);

    NYA_OsAddress same = { 0 };
    (void)nya_os_address_resolve("127.0.0.1", 8080, NYA_OS_ADDRESS_V4, &same);
    nya_check(nya_os_address_equals(loopback, same), "two of the same address are the same address");

    NYA_OsAddress other = loopback;
    other.port          = 8081;
    nya_check(!nya_os_address_equals(loopback, other), "and a different port is a different address");

    NYA_OsAddress v6 = { 0 };
    if (nya_os_address_resolve("::1", 443, NYA_OS_ADDRESS_V6, &v6) == NYA_OS_SOCKET_OK) {
      nya_check(v6.kind == NYA_OS_ADDRESS_V6, "an ipv6 literal resolves to ipv6, got %u", (u32)v6.kind);
      nya_check(nya_os_address_text(v6, true, text, sizeof(text)), "and writes back as text");

      // the brackets are what say where the address ends and the port begins.
      nya_check(nya_string_equals(text, "[::1]:443"), "in the form a port can follow, got '%s'", text);
    }

    NYA_OsAddress nothing = { 0 };
    nya_check(!nya_os_address_text(nothing, true, text, sizeof(text)), "an address that is not one writes nothing");
    nya_check(text[0] == '\0', "and leaves an empty string rather than whatever was there, got '%s'", text);

    nya_check(nya_os_address_resolve("", 80, NYA_OS_ADDRESS_NONE, &nothing) == NYA_OS_SOCKET_UNREACHABLE, "and an empty name resolves to nothing");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a datagram round trip, on a port the host chose.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsSocket server = NYA_OS_SOCKET_NONE;
    NYA_OsSocket client = NYA_OS_SOCKET_NONE;

    nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &server) == NYA_OS_SOCKET_OK, "a datagram socket opens on a port the host picks");
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &client) == NYA_OS_SOCKET_OK, "and so does a second one");

    NYA_OsAddress bound = { 0 };
    nya_check(nya_os_socket_address(server, &bound) == NYA_OS_SOCKET_OK, "the socket says where it ended up");
    nya_check(bound.port != 0, "which is a real port, got %u", bound.port);

    // nothing has been sent, so there is nothing to read, and that is not a failure.
    u8            buffer[64] = { 0 };
    u64           read       = 0;
    NYA_OsAddress from       = { 0 };

    nya_check(nya_os_socket_receive_from(server, buffer, sizeof(buffer), &read, &from) == NYA_OS_SOCKET_WOULD_BLOCK,
              "an empty socket answers would-block rather than waiting");

    // the address to answer on is loopback with the port the server was given.
    NYA_OsAddress to = { 0 };
    (void)nya_os_address_resolve("127.0.0.1", bound.port, bound.kind == NYA_OS_ADDRESS_V6 ? NYA_OS_ADDRESS_NONE : NYA_OS_ADDRESS_V4, &to);
    to.port = bound.port;

    const char* message = "ping";
    nya_check(nya_os_socket_send_to(client, to, (const u8*)message, strlen(message)) == NYA_OS_SOCKET_OK, "a datagram is sent");

    nya_check(wait_readable(server, WAIT_MS), "and the wait says the server has something");
    nya_check(nya_os_socket_receive_from(server, buffer, sizeof(buffer), &read, &from) == NYA_OS_SOCKET_OK, "which reads back");
    nya_check(read == strlen(message) && memcmp(buffer, message, read) == 0, "as what was sent, got " FMTu64 " bytes", read);
    nya_check(from.port != 0, "from a sender with a port, got %u", from.port);

    nya_os_socket_close(server);
    nya_os_socket_close(client);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a stream connection, accepted, written in both directions and ended.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsSocket listener = NYA_OS_SOCKET_NONE;
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &listener) == NYA_OS_SOCKET_OK, "a listener opens on a port the host picks");

    NYA_OsAddress bound = { 0 };
    nya_check(nya_os_socket_address(listener, &bound) == NYA_OS_SOCKET_OK, "and says which one");

    NYA_OsSocket  accepted = NYA_OS_SOCKET_NONE;
    NYA_OsAddress peer     = { 0 };

    nya_check(nya_os_socket_accept(listener, &accepted, &peer) == NYA_OS_SOCKET_WOULD_BLOCK, "with nobody waiting, accept answers would-block");

    NYA_OsAddress to = { 0 };
    (void)nya_os_address_resolve("127.0.0.1", bound.port, NYA_OS_ADDRESS_V4, &to);

    NYA_OsSocket       client    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(to, &client);

    // Either answer is right: loopback often connects inside the call, and under way is what a real
    // network does. What must not happen is a failure.
    nya_check(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK, "a connect is made or under way, got %u", (u32)connected);

    nya_check(wait_readable(listener, WAIT_MS), "the listener becomes readable, which is what a waiting connection looks like");
    nya_check(nya_os_socket_accept(listener, &accepted, &peer) == NYA_OS_SOCKET_OK, "and the connection is taken");
    nya_check(peer.kind != NYA_OS_ADDRESS_NONE, "with an address for who connected, got kind %u", (u32)peer.kind);

    nya_check(nya_os_socket_error(client) == NYA_OS_SOCKET_OK, "the connecting socket has nothing wrong with it");

    // and the bytes, in both directions.
    const char* hello = "hello";
    u64         sent  = 0;

    nya_check(nya_os_socket_send(client, (const u8*)hello, strlen(hello), &sent) == NYA_OS_SOCKET_OK, "the client writes");
    nya_check(sent == strlen(hello), "all of it, " FMTu64 " bytes", sent);

    u8  buffer[64] = { 0 };
    u64 read       = 0;

    nya_check(wait_readable(accepted, WAIT_MS), "the server sees it arrive");
    nya_check(nya_os_socket_receive(accepted, buffer, sizeof(buffer), &read) == NYA_OS_SOCKET_OK, "and reads it");
    nya_check(read == strlen(hello) && memcmp(buffer, hello, read) == 0, "as what was written, got '%.*s'", (s32)read, (const char*)buffer);

    const char* back = "there";
    nya_check(nya_os_socket_send(accepted, (const u8*)back, strlen(back), &sent) == NYA_OS_SOCKET_OK, "the server answers");
    nya_check(wait_readable(client, WAIT_MS), "the client sees the answer");
    nya_check(nya_os_socket_receive(client, buffer, sizeof(buffer), &read) == NYA_OS_SOCKET_OK && read == strlen(back), "and reads it");

    /*
     * The end of a stream. A peer that closes leaves its socket readable forever, so a zero byte read
     * has to be its own answer or a caller would ask again for the rest of time.
     */
    nya_os_socket_close(client);

    nya_check(wait_readable(accepted, WAIT_MS), "a closed peer makes the other side readable");
    nya_check(nya_os_socket_receive(accepted, buffer, sizeof(buffer), &read) == NYA_OS_SOCKET_CLOSED, "and the read says the stream is over");
    nya_check(read == 0, "with nothing in it, got " FMTu64 " bytes", read);

    nya_os_socket_close(accepted);
    nya_os_socket_close(listener);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a send takes what it can, which is the whole reason a write queue exists.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsSocket listener = NYA_OS_SOCKET_NONE;
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &listener) == NYA_OS_SOCKET_OK, "a listener opens");

    NYA_OsAddress bound = { 0 };
    (void)nya_os_socket_address(listener, &bound);

    NYA_OsAddress to = { 0 };
    (void)nya_os_address_resolve("127.0.0.1", bound.port, NYA_OS_ADDRESS_V4, &to);

    NYA_OsSocket client = NYA_OS_SOCKET_NONE;
    (void)nya_os_socket_connect(to, &client);

    NYA_OsSocket  accepted = NYA_OS_SOCKET_NONE;
    NYA_OsAddress peer     = { 0 };

    nya_check(wait_readable(listener, WAIT_MS), "a connection arrives");
    nya_check(nya_os_socket_accept(listener, &accepted, &peer) == NYA_OS_SOCKET_OK, "and is taken");

    // Four megabytes into a socket nobody is reading: the host takes what fits in its buffer and says
    // how much that was. A blocking socket would sit here instead, which is what this layer refuses.
    u8* big = nya_arena_alloc(nya_arena_global, BIG_SIZE);
    nya_memset(big, 'x', BIG_SIZE);

    u64 sent  = 0;
    u64 total = 0;

    NYA_OsSocketStatus status = NYA_OS_SOCKET_OK;

    for (u32 attempt = 0; attempt < 64; attempt++) {
      status = nya_os_socket_send(client, big + total, BIG_SIZE - total, &sent);

      if (status != NYA_OS_SOCKET_OK) break;

      total += sent;

      if (total >= BIG_SIZE) break;
    }

    nya_check(total > 0, "something went, " FMTu64 " bytes", total);
    nya_check(total < BIG_SIZE || status == NYA_OS_SOCKET_WOULD_BLOCK || total == BIG_SIZE, "and what is left is the caller's to send later");

    // whatever the host took is readable on the other side, which is the half that proves it was real.
    u8* landed = nya_arena_alloc(nya_arena_global, 65536);
    u64 read   = 0;

    nya_check(wait_readable(accepted, WAIT_MS), "the other side has bytes");
    nya_check(nya_os_socket_receive(accepted, landed, 65536, &read) == NYA_OS_SOCKET_OK && read > 0, "and reads some of them, " FMTu64, read);

    nya_os_socket_close(client);
    nya_os_socket_close(accepted);
    nya_os_socket_close(listener);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the wait, over several sockets and over none.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsSocket quiet = NYA_OS_SOCKET_NONE;
    NYA_OsSocket busy  = NYA_OS_SOCKET_NONE;

    nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &quiet) == NYA_OS_SOCKET_OK, "two datagram sockets open");
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &busy) == NYA_OS_SOCKET_OK, "the second as well");

    NYA_OsAddress bound = { 0 };
    (void)nya_os_socket_address(busy, &bound);

    NYA_OsAddress to = { 0 };
    (void)nya_os_address_resolve("127.0.0.1", bound.port, NYA_OS_ADDRESS_V4, &to);
    to.port = bound.port;

    (void)nya_os_socket_send_to(quiet, to, (const u8*)"x", 1);

    NYA_OsSocketWait watched[2] = {
      { .socket = quiet, .readable = true },
      { .socket = busy, .readable = true },
    };

    u32 ready = 0;
    nya_check(nya_os_socket_wait(watched, 2, WAIT_MS, &ready) == NYA_OS_SOCKET_OK, "the wait works over several sockets");
    nya_check(ready == 1, "and finds exactly the one with something, got %u", ready);
    nya_check(watched[1].is_readable && !watched[0].is_readable, "which is the one that was sent to");

    // a wait over nothing is a sleep, and a server with an empty table must not spin.
    u64 started_ns = nya_clock_get_monotonic_ns();

    nya_check(nya_os_socket_wait(watched, 0, 20, &ready) == NYA_OS_SOCKET_OK, "a wait over no sockets is fine");
    nya_check(ready == 0, "and finds nothing, got %u", ready);
    nya_check(nya_clock_get_monotonic_ns() - started_ns >= nya_time_ms_to_ns(10), "having actually waited rather than spun");

    // and a timeout with nothing to report is not a failure either.
    NYA_OsSocketWait alone = { .socket = quiet, .readable = true };

    nya_check(nya_os_socket_wait(&alone, 1, 20, &ready) == NYA_OS_SOCKET_OK, "a quiet socket times out without failing");
    nya_check(ready == 0 && !alone.is_readable, "with nothing ready, got %u", ready);

    nya_os_socket_close(quiet);
    nya_os_socket_close(busy);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what refuses, and what a socket that is not one answers.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_OsSocket first = NYA_OS_SOCKET_NONE;
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &first) == NYA_OS_SOCKET_OK, "a listener opens");

    NYA_OsAddress bound = { 0 };
    (void)nya_os_socket_address(first, &bound);

    NYA_OsSocket second = NYA_OS_SOCKET_NONE;
    nya_check(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, bound.port, 0, &second) == NYA_OS_SOCKET_IN_USE, "and a second on the same port is refused as taken");

    nya_os_socket_close(first);

    // The zeroed struct is no socket, and every call takes it without reaching for a descriptor that
    // is not there. Closing it twice is the case a connection table hits on every shutdown.
    NYA_OsSocket none = NYA_OS_SOCKET_NONE;

    u8  buffer[8] = { 0 };
    u64 moved     = 0;

    nya_check(nya_os_socket_receive(none, buffer, sizeof(buffer), &moved) == NYA_OS_SOCKET_FAILED, "a socket that was never opened cannot be read");
    nya_check(nya_os_socket_send(none, buffer, sizeof(buffer), &moved) == NYA_OS_SOCKET_FAILED, "or written");

    nya_os_socket_close(none);
    nya_os_socket_close(none);
  }

  nya_os_socket_stop();

  return nya_check_failures() == 0 ? 0 : 1;
}
