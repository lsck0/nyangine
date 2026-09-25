/**
 * The local control channel: names, the listener's lifetime, and what it does with a peer that
 * misbehaves. Everything here runs against a real socket in the runtime directory.
 **/

#include <unistd.h>

#include "SDL3/SDL_init.h"

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** Every listener in this file is named after the process, so two runs at once do not fight. */
static NYA_IpcName unique_name(NYA_Arena* arena, NYA_ConstCString suffix) {
  NYA_String* text = nya_string_sprintf(arena, "nya-test-%s-%d", suffix, (int)getpid());

  NYA_IpcName name = { 0 };
  NYA_EXPECT(nya_ipc_name_parse(nya_string_to_cstring(arena, text), &name));

  return name;
}

/** Drains a listener, counting what came out. Payload bytes are copied; the listener's buffer is reused. */
typedef struct {
  u32 connects;
  u32 disconnects;
  u32 data_events;
  u64 bytes;

  NYA_IpcPeerId last_peer;

  u8  last[256];
  u64 last_size;
} Drained;

static void drain(NYA_IpcListener* listener, Drained* out) {
  NYA_IpcEvent event = { 0 };

  // Bounded, exactly as the header says a caller must bound it.
  for (u32 step = 0; step < 64 && nya_ipc_listener_poll(listener, &event); step++) {
    switch (event.kind) {
      case NYA_IPC_EVENT_CONNECTED: {
        out->connects++;
        out->last_peer = event.peer;
      } break;

      case NYA_IPC_EVENT_DISCONNECTED: {
        out->disconnects++;
      } break;

      case NYA_IPC_EVENT_DATA: {
        out->data_events++;
        out->bytes     += event.size;
        out->last_peer  = event.peer;
        out->last_size  = nya_min(event.size, sizeof(out->last));

        nya_memcpy(out->last, event.data, out->last_size);
      } break;

      default: nya_assert(0, "an ipc event kind that does not exist");
    }
  }
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  nya_assert(SDL_Init(0), "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_ipc");
  defer      nya_arena_destroy(arena);

  printf("TEST: a name is parsed or it is not a name\n");
  {
    NYA_IpcName name = { 0 };

    NYA_EXPECT(nya_ipc_name_parse("nyangine", &name));
    nya_assert(nya_string_equals((NYA_ConstCString)name.text, "nyangine"));

    NYA_EXPECT(nya_ipc_name_parse("a.b_c-1", &name));

    // Everything that could climb out of the directory the name expands into, or end a pipe name early.
    nya_assert(!nya_ipc_name_parse("", &name).ok);
    nya_assert(!nya_ipc_name_parse(nullptr, &name).ok);
    nya_assert(!nya_ipc_name_parse("..", &name).ok);
    nya_assert(!nya_ipc_name_parse(".", &name).ok);
    nya_assert(!nya_ipc_name_parse("../etc/passwd", &name).ok);
    nya_assert(!nya_ipc_name_parse("a/b", &name).ok);
    nya_assert(!nya_ipc_name_parse("a\\b", &name).ok);
    nya_assert(!nya_ipc_name_parse("a b", &name).ok);
    nya_assert(!nya_ipc_name_parse("a\nb", &name).ok);

    char too_long[NYA_IPC_MAX_NAME + 8];
    for (u32 i = 0; i < sizeof(too_long) - 1; i++) too_long[i] = 'x';
    too_long[sizeof(too_long) - 1] = '\0';

    nya_assert(!nya_ipc_name_parse(too_long, &name).ok);

    printf("  eight illegal names refused, two legal ones parsed\n");
  }

  printf("TEST: a listener binds a name, and only one listener may\n");
  {
    NYA_IpcName name = unique_name(arena, "bind");

    NYA_IpcListener* listener = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &listener));

    nya_assert(nya_ipc_listener_endpoint(listener) != nullptr);
    nya_assert(nya_ipc_listener_connection_count(listener) == 0);

    NYA_IpcListener* second = nullptr;
    NYA_Error        taken  = nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &second);

    nya_assert(!taken.ok, "a second listener took a name that was already bound");
    nya_assert(taken.kind == NYA_ERROR_ALREADY_EXISTS);
    nya_assert(second == nullptr);

    // A name that is not a name never reaches the platform at all.
    NYA_IpcListener* unnamed = nullptr;
    nya_assert(!nya_ipc_listener_create(arena, (NYA_IpcOptions){ 0 }, &unnamed).ok);

    nya_assert(!nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name, .max_connections = NYA_IPC_MAX_CONNECTIONS + 1 }, &unnamed).ok);

    nya_ipc_listener_destroy(listener);

    // Destroying it gives the name back.
    NYA_IpcListener* again = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &again));
    nya_ipc_listener_destroy(again);

    printf("  bound, refused a second binding, and released the name on destroy\n");
  }

  printf("TEST: bytes move both ways\n");
  {
    NYA_IpcName name = unique_name(arena, "echo");

    NYA_IpcListener* listener = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &listener));
    defer nya_ipc_listener_destroy(listener);

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &client));
    defer nya_ipc_client_destroy(client);

    nya_assert(nya_ipc_client_is_connected(client));

    Drained drained = { 0 };
    drain(listener, &drained);

    nya_assert(drained.connects == 1, "the listener did not see the connection");
    nya_assert(nya_ipc_listener_connection_count(listener) == 1);

    NYA_IpcPeerId peer = drained.last_peer;
    nya_assert(nya_ipc_peer_is_set(peer));

    NYA_EXPECT(nya_ipc_client_send(client, (const u8*)"hello", 5));

    drained = (Drained){ 0 };
    drain(listener, &drained);

    nya_assert(drained.data_events == 1);
    nya_assert(drained.bytes == 5);
    nya_assert(nya_memcmp(drained.last, "hello", 5) == 0);
    nya_assert(nya_ipc_peer_equals(drained.last_peer, peer));

    NYA_EXPECT(nya_ipc_listener_send(listener, peer, (const u8*)"there", 5));

    u8  back[16] = { 0 };
    u64 got      = 0;

    // The listener queues and flushes opportunistically, so the bytes are already on their way.
    for (u32 attempt = 0; attempt < 64 && got == 0; attempt++) {
      NYA_EXPECT(nya_ipc_client_receive(client, back, sizeof(back), &got));
      if (got == 0) drain(listener, &drained);
    }

    nya_assert(got == 5, "the client got " FMTu64 " bytes back", got);
    nya_assert(nya_memcmp(back, "there", 5) == 0);

    printf("  five bytes up, five bytes back, on the same peer id\n");
  }

  printf("TEST: a peer id does not outlive its peer\n");
  {
    NYA_IpcName name = unique_name(arena, "stale");

    NYA_IpcListener* listener = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &listener));
    defer nya_ipc_listener_destroy(listener);

    NYA_IpcClient* first = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &first));

    Drained drained = { 0 };
    drain(listener, &drained);
    nya_assert(drained.connects == 1);

    NYA_IpcPeerId stale = drained.last_peer;

    nya_ipc_client_destroy(first);

    // The disconnect is noticed on the next poll that reads from it.
    for (u32 attempt = 0; attempt < 64 && nya_ipc_listener_connection_count(listener) > 0; attempt++) drain(listener, &drained);

    nya_assert(nya_ipc_listener_connection_count(listener) == 0, "the listener kept a dead connection");
    nya_assert(drained.disconnects >= 1);

    NYA_IpcClient* second = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &second));
    defer nya_ipc_client_destroy(second);

    drained = (Drained){ 0 };
    drain(listener, &drained);
    nya_assert(drained.connects == 1);

    NYA_IpcPeerId fresh = drained.last_peer;

    // The slot is the same one; the generation is not, which is the whole point of carrying one.
    nya_assert(fresh.index == stale.index);
    nya_assert(!nya_ipc_peer_equals(fresh, stale));

    NYA_Error to_the_dead = nya_ipc_listener_send(listener, stale, (const u8*)"x", 1);
    nya_assert(!to_the_dead.ok && to_the_dead.kind == NYA_ERROR_NOT_FOUND, "a stale id addressed the new peer");

    NYA_EXPECT(nya_ipc_listener_send(listener, fresh, (const u8*)"x", 1));

    // Disconnecting something that is not there is a no-op rather than a crash.
    nya_ipc_listener_disconnect(listener, stale);
    nya_ipc_listener_disconnect(listener, NYA_IPC_PEER_NONE);

    printf("  a reused slot refused the previous peer's id\n");
  }

  printf("TEST: the connection limit holds\n");
  {
    NYA_IpcName name = unique_name(arena, "full");

    NYA_IpcListener* listener = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name, .max_connections = 1 }, &listener));
    defer nya_ipc_listener_destroy(listener);

    NYA_IpcClient* kept = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &kept));
    defer nya_ipc_client_destroy(kept);

    Drained drained = { 0 };
    drain(listener, &drained);
    nya_assert(drained.connects == 1);

    /* The kernel accepts into its backlog, so connecting succeeds and the refusal arrives as an end of file the moment the listener looks. That is the behaviour worth pinning: a full listener says no immediately instead of leaving a tool waiting on a connection that will never be answered. */
    NYA_IpcClient* refused = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &refused));

    b8 closed = false;

    for (u32 attempt = 0; attempt < 64 && !closed; attempt++) {
      drain(listener, &drained);

      u8  ignored[8] = { 0 };
      u64 got        = 0;

      closed = !nya_ipc_client_receive(refused, ignored, sizeof(ignored), &got).ok;
    }

    nya_assert(closed, "a connection past the limit was left open");
    nya_assert(nya_ipc_listener_connection_count(listener) == 1);

    nya_ipc_client_destroy(refused);

    printf("  the second connection was closed and the first was untouched\n");
  }

  printf("TEST: a peer that does not read cannot grow the listener\n");
  {
    NYA_IpcName name = unique_name(arena, "greedy");

    NYA_IpcListener* listener = nullptr;
    NYA_EXPECT(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &listener));
    defer nya_ipc_listener_destroy(listener);

    NYA_IpcClient* silent = nullptr;
    NYA_EXPECT(nya_ipc_client_create(arena, name, &silent));
    defer nya_ipc_client_destroy(silent);

    Drained drained = { 0 };
    drain(listener, &drained);
    nya_assert(drained.connects == 1);

    NYA_IpcPeerId peer = drained.last_peer;

    u8* block = nya_arena_alloc(arena, NYA_IPC_BUFFER_BYTES);
    nya_memset(block, 'a', NYA_IPC_BUFFER_BYTES);

    /* Sent without the client ever reading. The kernel takes a socket buffer's worth and then the listener's own queue fills, at which point the send has to fail rather than allocate. */
    b8 refused = false;

    for (u32 attempt = 0; attempt < 256 && !refused; attempt++) {
      NYA_Error sent = nya_ipc_listener_send(listener, peer, block, NYA_IPC_BUFFER_BYTES);

      if (!sent.ok) {
        nya_assert(sent.kind == NYA_ERROR_OUT_OF_MEMORY, "the wrong error for a peer that is not reading");
        refused = true;
      }
    }

    nya_assert(refused, "the listener queued without bound for a peer that never read");
    nya_assert(nya_ipc_listener_connection_count(listener) == 1, "a slow peer was dropped instead of refused");

    printf("  the queue filled and the send was refused, with the connection left up\n");
  }

  printf("TEST: connecting to nothing is an error, not a crash\n");
  {
    NYA_IpcName name = unique_name(arena, "absent");

    NYA_IpcClient* client  = nullptr;
    NYA_Error      missing = nya_ipc_client_create(arena, name, &client);

    nya_assert(!missing.ok && missing.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(client == nullptr);

    // Both teardowns take what their setup can return, including nothing.
    nya_ipc_client_destroy(nullptr);
    nya_ipc_listener_destroy(nullptr);

    printf("  reported NOT_FOUND and both destroys took a null\n");
  }

  printf("PASSED: test_ipc (0 failures)\n");

  return EXIT_SUCCESS;
}
