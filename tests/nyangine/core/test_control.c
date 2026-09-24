/**
 * The control surface, driven exactly as another process would drive it: a real socket, real length
 * prefixed json, and no shortcuts into the engine's own state.
 *
 * Half of this file is the hostile half. A control socket takes commands, so every way a peer can be
 * wrong has to end in a reply or a dropped connection rather than in an assertion.
 **/

#include <unistd.h>

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** How many ticks a request is given to be answered before the test calls it lost. */
#define ANSWER_TICKS 64

/*
 * The reflection the generator would emit, written out by hand.
 *
 * src/genyarated/reflection.c is compiled with the game rather than with the engine, so a test that
 * links the shared engine object cannot name nya_reflect_of(anything). Writing the tables here costs
 * a dozen lines and buys two things the generated ones cannot: a struct shaped for exactly what this
 * file exercises, and a raw `char*` field, which nothing in the tree is annotated with yet and which
 * object.set has to refuse.
 */

typedef struct {
  u32   index;
  f32   amount;
  b8    enabled;
  char  label[16];
  char* borrowed;
} Knobs;

static const NYA_TypeReflection R_U32    = { .name = "u32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u32), .alignment = alignof(u32), .primitive = NYA_TYPE_U32 };
static const NYA_TypeReflection R_F32    = { .name = "f32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f32), .alignment = alignof(f32), .primitive = NYA_TYPE_F32 };
static const NYA_TypeReflection R_B8     = { .name = "b8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b8), .alignment = alignof(b8), .primitive = NYA_TYPE_B8 };
static const NYA_TypeReflection R_CHAR   = { .name = "char", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char), .alignment = alignof(char), .primitive = NYA_TYPE_CHAR };
static const NYA_TypeReflection R_STRING = { .name = "string", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char*), .alignment = alignof(char*), .primitive = NYA_TYPE_STRING };

static const NYA_TypeReflection R_LABEL = {
  .name = "char[16]", .kind = NYA_REFLECT_ARRAY, .size = 16, .alignment = alignof(char), .element = &R_CHAR, .element_count = 16,
};

static const NYA_ReflectField R_KNOBS_FIELDS[] = {
  { .name = "index", .type = &R_U32, .offset = offsetof(Knobs, index) },
  { .name = "amount", .type = &R_F32, .offset = offsetof(Knobs, amount) },
  { .name = "enabled", .type = &R_B8, .offset = offsetof(Knobs, enabled) },
  { .name = "label", .type = &R_LABEL, .offset = offsetof(Knobs, label) },
  { .name = "borrowed", .type = &R_STRING, .offset = offsetof(Knobs, borrowed) },
};

static const NYA_TypeReflection R_KNOBS = {
  .name        = "Knobs",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(Knobs),
  .alignment   = alignof(Knobs),
  .fields      = R_KNOBS_FIELDS,
  .field_count = 5,
};

/*
 * A window resize's payload, described by hand for the same reason as Knobs. Only the two sizes: the
 * window handle is an index a peer has no business writing.
 */
static const NYA_ReflectField R_RESIZED_FIELDS[] = {
  { .name = "width", .type = &R_U32, .offset = offsetof(NYA_WindowResizedEvent, width) },
  { .name = "height", .type = &R_U32, .offset = offsetof(NYA_WindowResizedEvent, height) },
};

static const NYA_TypeReflection R_RESIZED = {
  .name        = "NYA_WindowResizedEvent",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NYA_WindowResizedEvent),
  .alignment   = alignof(NYA_WindowResizedEvent),
  .fields      = R_RESIZED_FIELDS,
  .field_count = 2,
};

/** Larger than the union it would be read out of. */
static const NYA_TypeReflection R_OVERSIZED = {
  .name = "Oversized", .kind = NYA_REFLECT_STRUCT, .size = sizeof(NYA_Event) + 1, .alignment = alignof(NYA_Event),
};

static u32                    resized_events = 0;
static NYA_WindowResizedEvent last_resized   = { 0 };

static void on_resized(NYA_Event* event) {
  nya_assert(event->type == NYA_EVENT_WINDOW_RESIZED);

  resized_events++;
  last_resized = event->as_window_resized_event;
}

/** A json number, whichever integer type the parser gave it. */
static s64 number_field(const NYA_Object* object, NYA_ConstCString name) {
  NYA_Value* value = nya_object_get(object, (NYA_CString)name);

  nya_assert(value != nullptr, "no field '%s'", name);
  nya_assert(value->type == NYA_TYPE_S64 || value->type == NYA_TYPE_U64, "'%s' is not an integer", name);

  return value->type == NYA_TYPE_S64 ? value->as_s64 : (s64)value->as_u64;
}

static u32         control_messages   = 0;
static NYA_String* last_message_name  = nullptr;
static u64         last_message_value = 0;
static NYA_Arena*  test_arena         = nullptr;

/** What a program would write: an ordinary event hook, which never learns that a socket was involved. */
static void on_control_message(NYA_Event* event) {
  nya_assert(event->type == NYA_EVENT_CONTROL_MESSAGE);

  control_messages++;

  nya_string_clear(last_message_name);
  nya_string_extend(last_message_name, event->as_control_message_event.name);

  last_message_value = 0;

  if (event->as_control_message_event.body != nullptr) {
    NYA_Value* amount = nya_object_get(event->as_control_message_event.body, "amount");
    if (amount != nullptr && amount->type == NYA_TYPE_S64) last_message_value = (u64)amount->as_s64;
  }
}

static NYA_IpcName unique_name(NYA_ConstCString suffix) {
  NYA_String* text = nya_string_sprintf(test_arena, "nya-test-%s-%d", suffix, (int)getpid());

  NYA_IpcName name = { 0 };
  NYA_EXPECT(nya_ipc_name_parse(nya_string_to_cstring(test_arena, text), &name));

  return name;
}

/** Sends one framed document. */
static NYA_Error send_message(NYA_IpcClient* client, NYA_ConstCString json) {
  u64 length = strlen(json);

  u8 header[NYA_CONTROL_HEADER_BYTES] = { 0 };
  NYA_TRY(nya_control_frame_encode(length, header));

  NYA_TRY(nya_ipc_client_send(client, header, sizeof(header)));

  return nya_ipc_client_send(client, (const u8*)json, length);
}

/**
 * Ticks the control surface until it holds `count` connections. A client's connect returning is not the
 * server having accepted it: unix queues it in the listen backlog and a Windows pipe holds it in the one
 * waiting instance, and a check for "dropped" made before the accept passes without testing anything.
 * */
static void connections_wait_for(u32 count) {
  for (u32 tick = 0; tick < ANSWER_TICKS && nya_control_connection_count() != count; tick++) nya_system_control_tick();

  nya_assert(nya_control_connection_count() == count, "expected %u connections, have %u", count, nya_control_connection_count());
}

/**
 * Ticks the control surface until one whole reply is buffered, then parses it. Null when nothing came
 * back inside ANSWER_TICKS, which is how the hostile cases prove that a peer was dropped.
 * */
static NYA_Object* receive_message(NYA_IpcClient* client, NYA_String* buffer) {
  for (u32 tick = 0; tick < ANSWER_TICKS; tick++) {
    nya_system_control_tick();

    u8  chunk[1024] = { 0 };
    u64 got         = 0;

    if (!nya_ipc_client_is_connected(client)) break;
    if (!nya_ipc_client_receive(client, chunk, sizeof(chunk), &got).ok) break;

    for (u64 i = 0; i < got; i++) nya_string_push_back(buffer, chunk[i]);

    u64 length = 0;
    if (!nya_control_frame_decode(buffer->items, buffer->length, &length)) continue;
    if (buffer->length < NYA_CONTROL_HEADER_BYTES + length) continue;

    NYA_Object* message = nullptr;
    NYA_EXPECT(nya_deserialize(test_arena, buffer->items + NYA_CONTROL_HEADER_BYTES, length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &message));

    // Consumed, so a second call reads the next one rather than this one again.
    u64 whole = NYA_CONTROL_HEADER_BYTES + length;
    nya_memmove(buffer->items, buffer->items + whole, buffer->length - whole);
    buffer->length -= whole;

    return message;
  }

  return nullptr;
}

/** Send and receive, which is what every verb here does. */
static NYA_Object* call(NYA_IpcClient* client, NYA_String* buffer, NYA_ConstCString json) {
  NYA_EXPECT(send_message(client, json));

  return receive_message(client, buffer);
}

static b8 reply_is_ok(const NYA_Object* reply) {
  if (reply == nullptr) return false;

  NYA_Value* ok = nya_object_get(reply, "ok");

  return ok != nullptr && ok->type == NYA_TYPE_B8 && ok->as_b8;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  nya_assert(SDL_Init(0), "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  NYA_EXPECT(nya_system_events_init());
  defer nya_system_events_deinit();

  test_arena = nya_arena_create(.name = "test_control");
  defer nya_arena_destroy(test_arena);

  last_message_name = nya_string_create(test_arena);

  printf("TEST: framing is a pair\n");
  {
    u8  header[NYA_CONTROL_HEADER_BYTES] = { 0 };
    u64 length                           = 0;

    NYA_EXPECT(nya_control_frame_encode(1, header));
    nya_assert(nya_control_frame_decode(header, sizeof(header), &length) && length == 1);

    NYA_EXPECT(nya_control_frame_encode(NYA_CONTROL_MAX_MESSAGE_BYTES, header));
    nya_assert(nya_control_frame_decode(header, sizeof(header), &length) && length == NYA_CONTROL_MAX_MESSAGE_BYTES);

    // The two an encoder must refuse, so nothing downstream has to.
    nya_assert(!nya_control_frame_encode(0, header).ok);
    nya_assert(!nya_control_frame_encode(NYA_CONTROL_MAX_MESSAGE_BYTES + 1, header).ok);

    // An incomplete header is not a bad one.
    nya_assert(!nya_control_frame_decode(header, 3, &length) && length == 0);
    nya_assert(!nya_control_frame_decode(nullptr, 0, &length));

    // A length past the limit comes back as it was read, so the caller can tell the two apart.
    u8 huge[NYA_CONTROL_HEADER_BYTES] = { 0xFF, 0xFF, 0xFF, 0xFF };
    nya_assert(nya_control_frame_decode(huge, sizeof(huge), &length) && length == 0xFFFFFFFFULL);

    printf("  every length round trips, and the two illegal ones are refused\n");
  }

  printf("TEST: exposing is a pair too, and it checks what it is given\n");
  {
    Knobs knobs = { 0 };

    NYA_EXPECT(nya_control_expose("knobs", &R_KNOBS, &knobs));

    // Exposing the same name again replaces it rather than failing, so a reloaded module can re-expose.
    NYA_EXPECT(nya_control_expose("knobs", &R_KNOBS, &knobs));

    nya_assert(!nya_control_expose(nullptr, &R_KNOBS, &knobs).ok);
    nya_assert(!nya_control_expose("", &R_KNOBS, &knobs).ok);
    nya_assert(!nya_control_expose("knobs", nullptr, &knobs).ok);
    nya_assert(!nya_control_expose("knobs", &R_KNOBS, nullptr).ok);

    // A primitive is not a struct, and only a struct has fields to read.
    nya_assert(!nya_control_expose("scalar", &R_U32, &knobs).ok);

    char long_name[NYA_CONTROL_MAX_EXPOSED_NAME + 4];
    for (u32 i = 0; i < sizeof(long_name) - 1; i++) long_name[i] = 'n';
    long_name[sizeof(long_name) - 1] = '\0';

    nya_assert(!nya_control_expose(long_name, &R_KNOBS, &knobs).ok);

    nya_control_hide("knobs");
    nya_control_hide("never exposed");

    printf("  five bad exposures refused, a repeat replaced, and hiding an absent name is a no-op\n");
  }

  printf("TEST: a connected process reads and writes the program's own state\n");
  {
    NYA_IpcName name = unique_name("control");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name, .permissions = NYA_CONTROL_PERMISSION_ALL }));
    defer nya_system_control_deinit();

    nya_assert(nya_control_is_running());
    nya_assert(nya_control_endpoint() != nullptr);

    Knobs knobs = { .index = 1, .amount = 0.25F, .enabled = false };

    NYA_EXPECT(nya_control_expose("knobs", &R_KNOBS, &knobs));
    NYA_EXPECT(nya_control_expose("other", &R_KNOBS, &knobs));

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);

    NYA_String* buffer = nya_string_create(test_arena);

    NYA_Object* hello = call(client, buffer, "{\"op\":\"hello\",\"id\":7}");

    nya_assert(reply_is_ok(hello), "hello was not answered");
    nya_assert(nya_object_get(hello, "id")->as_s64 == 7, "the id was not echoed");
    nya_assert(nya_object_get(hello, "protocol")->as_u64 == NYA_CONTROL_PROTOCOL_VERSION);
    nya_assert(nya_object_get(hello, "objects")->type == NYA_TYPE_ARRAY);
    nya_assert(nya_object_get(hello, "events")->type == NYA_TYPE_ARRAY);
    nya_assert(nya_object_get(hello, "permissions")->as_array.length == 2);

    nya_assert(nya_control_connection_count() == 1);

    NYA_Object* listed = call(client, buffer, "{\"op\":\"object.list\"}");
    nya_assert(reply_is_ok(listed));
    nya_assert(nya_object_get(listed, "objects")->as_array.length == 2);

    NYA_Object* read = call(client, buffer, "{\"op\":\"object.get\",\"name\":\"knobs\"}");
    nya_assert(reply_is_ok(read));

    NYA_Value* value = nya_object_get(read, "value");
    nya_assert(value != nullptr && value->type == NYA_TYPE_OBJECT);
    nya_assert(nya_object_get(&value->as_object, "index")->as_u32 == 1);

    NYA_Object* written = call(client, buffer, "{\"op\":\"object.set\",\"name\":\"knobs\",\"value\":{\"index\":42}}");
    nya_assert(reply_is_ok(written));

    // The live struct changed, and the field that was not named did not.
    nya_assert(knobs.index == 42, "object.set did not reach the instance");
    nya_assert(knobs.amount == 0.25F, "object.set touched a field it was not given");

    // A char array is the struct's own storage, so writing one copies rather than borrowing.
    NYA_Object* named = call(client, buffer, "{\"op\":\"object.set\",\"name\":\"knobs\",\"value\":{\"label\":\"live\",\"enabled\":true,\"amount\":0.5}}");
    nya_assert(reply_is_ok(named));
    nya_assert(nya_string_equals((NYA_ConstCString)knobs.label, "live"));
    nya_assert(knobs.enabled);
    nya_assert(knobs.amount == 0.5F);

    /* A `char*` field is the one thing a write may not touch. nya_reflect_from_object would store the request's own pointer, and that pointer belongs to the scratch the next tick reuses, so the live struct would be left holding memory that is about to be something else. */
    NYA_Object* borrowed = call(client, buffer, "{\"op\":\"object.set\",\"name\":\"knobs\",\"value\":{\"borrowed\":\"gone next frame\"}}");
    nya_assert(!reply_is_ok(borrowed), "a string field was written over the control socket");
    nya_assert(knobs.borrowed == nullptr, "a string field was written over the control socket");

    printf("  hello, object.list, object.get, two partial object.sets, and one string refused\n");
  }

  printf("TEST: an outside process raises an engine event, and a hook takes it\n");
  {
    NYA_IpcName name = unique_name("dispatch");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name, .permissions = NYA_CONTROL_PERMISSION_DISPATCH }));
    defer nya_system_control_deinit();

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_CONTROL_MESSAGE,
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .fn         = nya_callback(on_control_message),
    };

    nya_event_hook_register(hook);
    defer nya_event_hook_unregister(hook);

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);

    NYA_String* buffer = nya_string_create(test_arena);

    control_messages = 0;

    NYA_Object* dispatched =
        call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"CONTROL_MESSAGE\",\"name\":\"wave\",\"body\":{\"amount\":3}}");

    nya_assert(reply_is_ok(dispatched), "the dispatch was refused");
    nya_assert(control_messages == 1, "the hook did not run");
    nya_assert(nya_string_equals(last_message_name, "wave"));
    nya_assert(last_message_value == 3, "the body did not reach the hook");

    // An ordinary engine event, with no payload to describe.
    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"QUIT\"}")));

    // Everything a dispatch must refuse.
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"NOT_AN_EVENT\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"INVALID\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"LIFECYCLE_EVENTS_BEGIN\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"CONTROL_MESSAGE\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"CONTROL_MESSAGE\",\"name\":\"\"}")));

    // Reading is always allowed; writing is not, and this surface was not given WRITE.
    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"hello\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"object.set\",\"name\":\"knobs\",\"value\":{}}")));

    // The queue is drained, so nothing the dispatches above left behind reaches the next test.
    NYA_Event drained = { 0 };
    while (nya_system_event_poll(&drained)) {}

    printf("  one CONTROL_MESSAGE delivered with its body, six bad dispatches refused, one write refused\n");
  }

  printf("TEST: a subscriber is told what the engine raised\n");
  {
    NYA_IpcName name = unique_name("subscribe");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name }));
    defer nya_system_control_deinit();

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);

    NYA_String* buffer = nya_string_create(test_arena);

    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\",\"types\":[\"QUIT\"]}")));

    // Raised by the program itself, which is the case that matters: the socket is an observer.
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_QUIT });

    NYA_Object* pushed = receive_message(client, buffer);

    nya_assert(pushed != nullptr, "the subscriber was told nothing");
    nya_assert(nya_object_get(pushed, "id") == nullptr, "a pushed event carried an id, so it reads as a reply");
    nya_assert(nya_string_equals(nya_object_get(pushed, "op")->as_string, "event"));
    nya_assert(nya_string_equals(nya_object_get(pushed, "type")->as_string, "QUIT"));

    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.unsubscribe\",\"types\":[\"QUIT\"]}")));

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_QUIT });

    // Nothing arrives now, so the poll runs out of ticks and answers null.
    nya_assert(receive_message(client, buffer) == nullptr, "an unsubscribed event was still pushed");

    // A subscription list has to be a list of names this build knows.
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\",\"types\":\"QUIT\"}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\",\"types\":[7]}")));
    nya_assert(!reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\",\"types\":[\"NOPE\"]}")));

    NYA_Event drained = { 0 };
    while (nya_system_event_poll(&drained)) {}

    printf("  one event pushed, one not pushed after unsubscribing, four bad subscriptions refused\n");
  }

  printf("TEST: an exposed event carries its payload both ways, and a hidden one carries none\n");
  {
    // the ones that would read past the event or name nothing, refused before the socket is up.
    nya_assert(!nya_control_expose_event(NYA_EVENT_INVALID, &R_RESIZED).ok);
    nya_assert(!nya_control_expose_event(NYA_EVENT_COUNT, &R_RESIZED).ok);
    nya_assert(!nya_control_expose_event(NYA_EVENT_WINDOW_RESIZED, nullptr).ok);
    nya_assert(!nya_control_expose_event(NYA_EVENT_WINDOW_RESIZED, &R_U32).ok);
    nya_assert(!nya_control_expose_event(NYA_EVENT_WINDOW_RESIZED, &R_OVERSIZED).ok);

    // and hiding what was never exposed, or is not an event at all, is a no-op.
    nya_control_hide_event(NYA_EVENT_WINDOW_MOVED);
    nya_control_hide_event(NYA_EVENT_COUNT);

    NYA_IpcName name = unique_name("payload");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name, .permissions = NYA_CONTROL_PERMISSION_DISPATCH }));
    defer nya_system_control_deinit();

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);

    NYA_String* buffer = nya_string_create(test_arena);

    NYA_EXPECT(nya_control_expose_event(NYA_EVENT_WINDOW_RESIZED, &R_RESIZED));

    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.subscribe\",\"types\":[\"WINDOW_RESIZED\"]}")));

    // ── Out: what the program raised reaches the peer with its fields read by reflection ──
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_WINDOW_RESIZED, .as_window_resized_event = { .width = 640, .height = 480 } });

    NYA_Object* pushed = receive_message(client, buffer);
    nya_assert(pushed != nullptr, "the subscriber was told nothing");
    nya_assert(nya_string_equals(nya_object_get(pushed, "type")->as_string, "WINDOW_RESIZED"));

    NYA_Value* payload = nya_object_get(pushed, "payload");
    nya_assert(payload != nullptr && payload->type == NYA_TYPE_OBJECT, "an exposed event was pushed without its payload");
    nya_assert(number_field(&payload->as_object, "width") == 640, "the payload's width did not survive the wire");
    nya_assert(number_field(&payload->as_object, "height") == 480, "the payload's height did not survive the wire");

    // hidden, the event still reaches a subscriber, but as a bare name: nothing is read out of it.
    nya_control_hide_event(NYA_EVENT_WINDOW_RESIZED);
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_WINDOW_RESIZED, .as_window_resized_event = { .width = 800, .height = 600 } });

    NYA_Object* bare = receive_message(client, buffer);
    nya_assert(bare != nullptr, "hiding the payload must not hide the event");
    nya_assert(nya_object_get(bare, "payload") == nullptr, "a hidden event's payload was still described");

    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.unsubscribe\",\"types\":[\"WINDOW_RESIZED\"]}")));

    // ── In: a peer's dispatch fills the payload, and only while it is exposed ──
    NYA_EventHook hook = {
      .event_type = NYA_EVENT_WINDOW_RESIZED,
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .fn         = nya_callback(on_resized),
    };

    nya_event_hook_register(hook);
    defer nya_event_hook_unregister(hook);

    NYA_EXPECT(nya_control_expose_event(NYA_EVENT_WINDOW_RESIZED, &R_RESIZED));

    resized_events = 0;
    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"WINDOW_RESIZED\",\"payload\":{\"width\":1024,\"height\":768}}")));

    nya_assert(resized_events == 1, "the dispatch never reached the hook");
    nya_assert(last_resized.width == 1024 && last_resized.height == 768, "the hook saw %ux%u", last_resized.width, last_resized.height);

    nya_control_hide_event(NYA_EVENT_WINDOW_RESIZED);

    nya_assert(reply_is_ok(call(client, buffer, "{\"op\":\"event.dispatch\",\"type\":\"WINDOW_RESIZED\",\"payload\":{\"width\":1024,\"height\":768}}")));

    nya_assert(resized_events == 2, "a hidden event type must still dispatch");
    nya_assert(last_resized.width == 0 && last_resized.height == 0, "a hidden payload was written anyway: %ux%u", last_resized.width, last_resized.height);

    NYA_Event drained = { 0 };
    while (nya_system_event_poll(&drained)) {}

    printf("  five bad descriptions refused, a payload out and in, and neither once hidden\n");
  }

  printf("TEST: nothing a peer can send reaches an assertion\n");
  {
    NYA_IpcName name = unique_name("hostile");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name, .permissions = NYA_CONTROL_PERMISSION_ALL }));
    defer nya_system_control_deinit();

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);

    NYA_String* buffer = nya_string_create(test_arena);

    // Every one of these is answered, and the connection survives all of them.
    NYA_ConstCString nonsense[] = {
      "{}",
      "{\"op\":7}",
      "{\"op\":\"\"}",
      "{\"op\":\"nope\"}",
      "{\"op\":\"object.get\"}",
      "{\"op\":\"object.get\",\"name\":\"nothing here\"}",
      "{\"op\":\"object.set\",\"name\":\"nothing here\",\"value\":{}}",
      "{\"op\":\"object.set\",\"name\":\"knobs\"}",
      "not json at all",
      "[1,2,3]",
      "{\"op\":\"hello\",",
      "\x01\x02\x03\x04",
    };

    for (u32 i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++) {
      NYA_Object* reply = call(client, buffer, nonsense[i]);

      nya_assert(reply != nullptr, "'%s' was not answered at all", nonsense[i]);
      nya_assert(!reply_is_ok(reply), "'%s' was accepted", nonsense[i]);
      nya_assert(nya_object_get(reply, "error") != nullptr, "'%s' was refused with no reason", nonsense[i]);
    }

    nya_assert(nya_control_connection_count() == 1, "the connection did not survive well formed nonsense");

    printf("  twelve malformed requests answered, with the connection still up\n");
  }

  printf("TEST: a peer that lies about a length is dropped\n");
  {
    NYA_IpcName name = unique_name("liar");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name }));
    defer nya_system_control_deinit();

    NYA_IpcClient* client = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &client));
    defer nya_ipc_client_destroy(client);
    connections_wait_for(1);

    // A header announcing more than a message may be, and not a byte of body behind it.
    u8 header[NYA_CONTROL_HEADER_BYTES] = { 0xFF, 0xFF, 0xFF, 0xFF };
    NYA_EXPECT(nya_ipc_client_send(client, header, sizeof(header)));

    for (u32 tick = 0; tick < ANSWER_TICKS && nya_control_connection_count() > 0; tick++) nya_system_control_tick();

    nya_assert(nya_control_connection_count() == 0, "a peer that announced an impossible length was kept");

    // And a zero length, which is the other end of the same rule.
    NYA_IpcClient* second = nullptr;
    NYA_EXPECT(nya_ipc_client_create(test_arena, name, &second));
    defer nya_ipc_client_destroy(second);
    connections_wait_for(1);

    u8 empty[NYA_CONTROL_HEADER_BYTES] = { 0, 0, 0, 0 };
    NYA_EXPECT(nya_ipc_client_send(second, empty, sizeof(empty)));

    for (u32 tick = 0; tick < ANSWER_TICKS && nya_control_connection_count() > 0; tick++) nya_system_control_tick();

    nya_assert(nya_control_connection_count() == 0, "a peer that announced a zero length was kept");

    printf("  both impossible lengths dropped the connection instead of reading a byte\n");
  }

  printf("TEST: the subsystem is off unless it is turned on\n");
  {
    nya_assert(!nya_control_is_running());
    nya_assert(nya_control_endpoint() == nullptr);
    nya_assert(nya_control_connection_count() == 0);

    // Every one of these is a no-op rather than a fault when nothing is running.
    nya_system_control_tick();
    nya_system_control_deinit();
    nya_system_control_deinit();

    NYA_IpcName name = unique_name("twice");

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name }));
    defer nya_system_control_deinit();

    NYA_Error again = nya_system_control_init((NYA_ControlConfig){ .name = name });
    nya_assert(!again.ok && again.kind == NYA_ERROR_ALREADY_EXISTS);

    printf("  off is free, tick and deinit are no-ops, and a second init is refused\n");
  }

  printf("PASSED: test_control (0 failures)\n");

  return EXIT_SUCCESS;
}
