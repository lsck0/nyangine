/**
 * The fault interposer: every operation class fails the way its rule says, only for what the plan
 * follows, the same way for the same seed, and not at all once the plan is detached.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Long enough that a lost loopback datagram is a bug rather than bad luck. */
#define WAIT_MS 200

/** Calls a probabilistic rule is sampled over. Enough that a 25% rule lands well inside its bounds. */
#define SAMPLE_CALLS 4000

static b8 wait_readable(NYA_OsSocket socket) {
    NYA_OsSocketWait watched = { .socket = socket, .readable = true };
    u32              ready   = 0;

    return nya_os_socket_wait(&watched, 1, WAIT_MS, &ready) == NYA_OS_SOCKET_OK && watched.is_readable;
}

/** Every datagram waiting on `socket` within the wait, into `out`, as their first bytes. */
static u32 drain_datagrams(NYA_OsSocket socket, OUT u8* out, u32 capacity) {
    u32 count = 0;

    while (count < capacity && wait_readable(socket)) {
        u8            buffer[64];
        u64           size = 0;
        NYA_OsAddress from = { 0 };

        NYA_OsSocketStatus status = nya_os_socket_receive_from(socket, buffer, sizeof(buffer), &size, &from);
        if (status == NYA_OS_SOCKET_WOULD_BLOCK) continue;
        if (status != NYA_OS_SOCKET_OK || size == 0) break;

        out[count++] = buffer[0];
    }

    return count;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_fault");
    defer      nya_arena_destroy(arena);

    NYA_String* temp = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(arena, &temp));

    NYA_CString root = nya_string_to_cstring(arena, nya_path_join(arena, nya_string_to_cstring(arena, temp), "nyangine-test-fault"));
    (void)nya_filesystem_delete_recursive(root);
    NYA_EXPECT(nya_filesystem_create_directory(root));
    defer (void)nya_filesystem_delete_recursive(root);

    NYA_CString followed   = nya_string_to_cstring(arena, nya_path_join(arena, root, "followed.bin"));
    NYA_CString unfollowed = nya_string_to_cstring(arena, nya_path_join(arena, nya_string_to_cstring(arena, temp), "nyangine-unfollowed-by-test-fault.bin"));
    defer (void)nya_filesystem_delete(unfollowed);

    // TEST: a plan attaches once per thread, and detaching twice is harmless.
    {
        NYA_OsFaultPlan first  = { 0 };
        NYA_OsFaultPlan second = { 0 };

        nya_check(nya_os_fault_plan_attach(&first), "a plan attaches to a thread with none");
        nya_check(!nya_os_fault_plan_attach(&second), "a second plan is refused rather than replacing the first");
        nya_check(nya_os_fault_plan_current() == &first, "the first plan is the one attached");

        nya_os_fault_plan_detach();
        nya_os_fault_plan_detach();
        nya_check(nya_os_fault_plan_current() == nullptr, "detached, the thread runs under no plan");
    }

    // TEST: a scripted write fault fails exactly the Nth write, and only under the followed prefix.
    {
        NYA_OsFaultPlan plan = { .seed = 1, .path_prefix = root };
        plan.rules[NYA_OS_FAULT_FILE_WRITE] = (NYA_OsFaultRule){ .fail_call = 2, .fail_kind = NYA_OS_FAULT_NO_SPACE };

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");

        NYA_File file = { 0 };
        NYA_EXPECT(nya_file_open(followed, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));

        NYA_Error first  = nya_file_write_bytes(&file, (const u8*)"one", 3);
        NYA_Error second = nya_file_write_bytes(&file, (const u8*)"two", 3);
        NYA_Error third  = nya_file_write_bytes(&file, (const u8*)"six", 3);
        nya_file_close(&file);

        nya_check(first.ok, "the first write goes through: %s", first.message);
        nya_check(!second.ok && second.kind == NYA_ERROR_IO, "the second is the disk filling up, got %s", NYA_ERRORKIND_NAME_MAP[second.kind]);
        nya_check(third.ok, "a scripted fault is one fault, and the third write goes through: %s", third.message);
        nya_check(plan.injected[NYA_OS_FAULT_FILE_WRITE] == 1, "one write fault was injected, got " FMTu64, plan.injected[NYA_OS_FAULT_FILE_WRITE]);

        NYA_File other = { 0 };
        NYA_EXPECT(nya_file_open(unfollowed, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &other));
        for (u32 i = 0; i < 4; i++) nya_check(nya_file_write_bytes(&other, (const u8*)"x", 1).ok, "a file outside the prefix is never faulted");
        nya_file_close(&other);

        nya_check(plan.calls[NYA_OS_FAULT_FILE_WRITE] == 3, "only the followed file's writes were counted, got " FMTu64, plan.calls[NYA_OS_FAULT_FILE_WRITE]);

        nya_os_fault_plan_detach();
    }

    // TEST: short writes and short reads are absorbed by the loops above them, so the bytes still round trip.
    {
        NYA_OsFaultPlan plan = { .seed = 2, .path_prefix = root };
        plan.rules[NYA_OS_FAULT_FILE_WRITE] = (NYA_OsFaultRule){ .probability_ppm = NYA_OS_FAULT_PPM, .kinds = 1U << NYA_OS_FAULT_SHORT };
        plan.rules[NYA_OS_FAULT_FILE_READ]  = (NYA_OsFaultRule){ .probability_ppm = NYA_OS_FAULT_PPM, .kinds = 1U << NYA_OS_FAULT_SHORT };

        NYA_ConstCString content = "a sentence long enough that every write of it comes up short several times over";

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");
        NYA_Error written = nya_file_write(followed, content);
        NYA_String* back  = nya_string_create(arena);
        NYA_Error   read  = nya_file_read(followed, back);
        nya_os_fault_plan_detach();

        nya_check(written.ok && read.ok, "a short write and a short read are not failures");
        nya_check(nya_string_equals(back, content), "and the content survives them, got '%s'", nya_string_to_cstring(arena, back));
        nya_check(plan.injected[NYA_OS_FAULT_FILE_WRITE] > 1, "the writes did come up short, " FMTu64 " times", plan.injected[NYA_OS_FAULT_FILE_WRITE]);
    }

    // TEST: a flipped read differs from what is on disk in exactly one bit.
    {
        NYA_EXPECT(nya_file_write(followed, "flip"));

        NYA_OsFaultPlan plan = { .seed = 3, .path_prefix = root };
        plan.rules[NYA_OS_FAULT_FILE_READ] = (NYA_OsFaultRule){ .fail_call = 1, .fail_kind = NYA_OS_FAULT_FLIP };

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");
        NYA_String* back = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(followed, back));
        nya_os_fault_plan_detach();

        u32 bits = 0;
        for (u64 i = 0; i < back->length && i < 4; i++) bits += (u32)__builtin_popcount((u8)(back->items[i] ^ (u8)"flip"[i]));

        nya_check(back->length == 4 && bits == 1, "one bit turned over, got " FMTu32 " over " FMTu64 " bytes", bits, back->length);
    }

    // TEST: a sync or a rename that fails leaves an atomic replace's old content in place.
    {
        NYA_EXPECT(nya_file_write_atomic(followed, "before"));

        for (u32 op = NYA_OS_FAULT_FILE_SYNC; op <= NYA_OS_FAULT_FILE_RENAME; op++) {
            NYA_OsFaultPlan plan = { .seed = 4, .path_prefix = root };
            plan.rules[op]       = (NYA_OsFaultRule){ .fail_call = 1, .fail_kind = NYA_OS_FAULT_IO };

            nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");
            NYA_Error replaced = nya_file_write_atomic(followed, "after");
            nya_os_fault_plan_detach();

            NYA_String* back = nya_string_create(arena);
            NYA_EXPECT(nya_file_read(followed, back));

            nya_check(!replaced.ok, "op " FMTu32 ": the replace reports the fault", op);
            nya_check(nya_string_equals(back, "before"), "op " FMTu32 ": and the old content is intact, got '%s'", op, nya_string_to_cstring(arena, back));
        }
    }

    // TEST: the same seed draws the same faults, and a different seed different ones.
    {
        u64 digests[3] = { 0 };
        u64 seeds[3]   = { 7, 7, 8 };

        for (u32 run = 0; run < 3; run++) {
            NYA_OsFaultPlan plan = { .seed = seeds[run] };
            plan.rules[NYA_OS_FAULT_ALLOCATION] = (NYA_OsFaultRule){ .probability_ppm = NYA_OS_FAULT_PPM / 4 };

            nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");
            for (u32 i = 0; i < SAMPLE_CALLS; i++) (void)nya_os_fault_decide(NYA_OS_FAULT_ALLOCATION);
            nya_os_fault_plan_detach();

            digests[run] = plan.digest;

            u64 injected = plan.injected[NYA_OS_FAULT_ALLOCATION];
            nya_check(injected > SAMPLE_CALLS / 5 && injected < SAMPLE_CALLS / 3, "a quarter of the calls fault, got " FMTu64 " of %d", injected, SAMPLE_CALLS);
        }

        nya_check(digests[0] == digests[1], "one seed replays the same faults");
        nya_check(digests[0] != digests[2], "another seed draws different ones");
    }

    // TEST: seeded random bytes repeat per seed, and a random fault is a refusal.
    {
        u8 first[32]  = { 0 };
        u8 second[32] = { 0 };

        NYA_OsFaultPlan a = { .seed = 9, .random_seeded = true };
        nya_check(nya_os_fault_plan_attach(&a), "the plan attaches");
        nya_check(nya_os_random_bytes(first, sizeof(first)), "expected %s", "nya_os_random_bytes(first, sizeof(first))");
        nya_os_fault_plan_detach();

        NYA_OsFaultPlan b = { .seed = 9, .random_seeded = true };
        b.rules[NYA_OS_FAULT_RANDOM] = (NYA_OsFaultRule){ .fail_call = 2, .fail_kind = NYA_OS_FAULT_FAIL };
        nya_check(nya_os_fault_plan_attach(&b), "the plan attaches");
        nya_check(nya_os_random_bytes(second, sizeof(second)), "expected %s", "nya_os_random_bytes(second, sizeof(second))");
        nya_check(!nya_os_random_bytes(second, sizeof(second)), "the scripted draw is refused");
        nya_os_fault_plan_detach();

        nya_check(memcmp(first, second, sizeof(first)) == 0, "the same seed hands out the same bytes");
    }

    // TEST: the simulated clocks move only when told, and the wall clock alone can go backwards.
    {
        NYA_OsFaultPlan plan = { .time_simulated = true };
        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");

        u64 monotonic = nya_clock_get_monotonic_ns();
        u64 wall      = nya_clock_get_timestamp_ns();
        nya_check(monotonic == NYA_OS_FAULT_MONOTONIC_ORIGIN_NS, "the monotonic clock starts at its origin, got " FMTu64, monotonic);
        nya_check(wall == NYA_OS_FAULT_WALL_ORIGIN_NS, "the wall clock at its own, got " FMTu64, wall);

        nya_os_time_sleep_ms(250);
        nya_check(nya_clock_get_monotonic_ns() == monotonic + 250'000'000ULL, "a sleep moves the clock instead of waiting");

        nya_os_fault_time_skew_wall(&plan, -60LL * 1'000'000'000LL);
        nya_check(nya_clock_get_timestamp_ns() < wall, "a skew back puts the wall clock before where it started");
        nya_check(nya_clock_get_monotonic_ns() >= monotonic + 250'000'000ULL, "and leaves the monotonic one alone");

        nya_os_fault_plan_detach();
        nya_check(nya_clock_get_timestamp_s() > NYA_OS_FAULT_WALL_ORIGIN_NS / 1'000'000'000ULL + 365ULL * 86'400ULL * 20ULL, "detached, the host's clock is back");
    }

    // TEST: datagrams on a followed socket are dropped, doubled, flipped and held back.
    {
        NYA_OsFaultPlan plan = { .seed = 11, .sockets = NYA_OS_FAULT_SOCKETS_DATAGRAM };
        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");

        NYA_OsSocket receiver = NYA_OS_SOCKET_NONE;
        NYA_OsSocket sender   = NYA_OS_SOCKET_NONE;
        nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &receiver) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &receiver) == NYA_OS_SOCKET_OK");
        nya_check(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &sender) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &sender) == NYA_OS_SOCKET_OK");
        defer nya_os_socket_close(receiver);
        defer nya_os_socket_close(sender);

        NYA_OsAddress bound = { 0 };
        nya_check(nya_os_socket_address(receiver, &bound) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_address(receiver, &bound) == NYA_OS_SOCKET_OK");

        NYA_OsAddress to = { 0 };
        nya_check(nya_os_address_resolve("127.0.0.1", bound.port, NYA_OS_ADDRESS_V4, &to) == NYA_OS_SOCKET_OK, "loopback resolves");

        static const NYA_OsFaultKind KINDS[] = { NYA_OS_FAULT_DROP, NYA_OS_FAULT_DUPLICATE, NYA_OS_FAULT_FLIP };
        static const u32             WANT[]  = { 0, 2, 1 };

        for (u32 i = 0; i < nya_carray_length(KINDS); i++) {
            plan.rules[NYA_OS_FAULT_SOCKET_SEND_TO] = (NYA_OsFaultRule){ .fail_call = plan.calls[NYA_OS_FAULT_SOCKET_SEND_TO] + 1, .fail_kind = KINDS[i] };

            u8 payload = 0x40;
            nya_check(nya_os_socket_send_to(sender, to, &payload, 1) == NYA_OS_SOCKET_OK, "a faulted send still says the host took it");

            u8  got[8] = { 0 };
            u32 count  = drain_datagrams(receiver, got, nya_carray_length(got));

            nya_check(count == WANT[i], "kind " FMTu32 ": " FMTu32 " datagrams arrive, got " FMTu32, (u32)KINDS[i], WANT[i], count);
            if (KINDS[i] == NYA_OS_FAULT_FLIP) nya_check(count == 1 && __builtin_popcount((u8)(got[0] ^ payload)) == 1, "the flipped one differs in a bit");
        }

        // held, then overtaken by the next one, then released: the order on the wire is reversed.
        plan.rules[NYA_OS_FAULT_SOCKET_SEND_TO] = (NYA_OsFaultRule){ .fail_call = plan.calls[NYA_OS_FAULT_SOCKET_SEND_TO] + 1, .fail_kind = NYA_OS_FAULT_DELAY };

        u8 early = 1;
        u8 late  = 2;
        nya_check(nya_os_socket_send_to(sender, to, &early, 1) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_send_to(sender, to, &early, 1) == NYA_OS_SOCKET_OK");
        nya_check(plan.held_count == 1, "the delayed datagram is held");
        nya_check(nya_os_socket_send_to(sender, to, &late, 1) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_send_to(sender, to, &late, 1) == NYA_OS_SOCKET_OK");
        nya_os_fault_datagrams_flush(&plan);

        u8  got[4] = { 0 };
        u32 count  = drain_datagrams(receiver, got, nya_carray_length(got));
        nya_check(count == 2 && got[0] == late && got[1] == early, "the held one arrives after the one sent behind it");

        nya_os_fault_plan_detach();
    }

    // TEST: a stream follows its rules on the accepted side only: a refused accept, a short send, and a reset.
    {
        NYA_OsFaultPlan plan = { .seed = 12, .sockets = NYA_OS_FAULT_SOCKETS_ACCEPTED };
        plan.rules[NYA_OS_FAULT_SOCKET_ACCEPT] = (NYA_OsFaultRule){ .fail_call = 1, .fail_kind = NYA_OS_FAULT_FAIL };
        plan.rules[NYA_OS_FAULT_SOCKET_SEND]   = (NYA_OsFaultRule){ .probability_ppm = NYA_OS_FAULT_PPM, .kinds = 1U << NYA_OS_FAULT_SHORT };

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");

        NYA_OsSocket listener = NYA_OS_SOCKET_NONE;
        nya_check(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &listener) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &listener) == NYA_OS_SOCKET_OK");
        defer nya_os_socket_close(listener);

        NYA_OsAddress bound = { 0 };
        nya_check(nya_os_socket_address(listener, &bound) == NYA_OS_SOCKET_OK, "expected %s", "nya_os_socket_address(listener, &bound) == NYA_OS_SOCKET_OK");

        NYA_OsAddress to = { 0 };
        nya_check(nya_os_address_resolve("127.0.0.1", bound.port, NYA_OS_ADDRESS_V4, &to) == NYA_OS_SOCKET_OK, "loopback resolves");

        NYA_OsSocket client = NYA_OS_SOCKET_NONE;
        NYA_OsSocketStatus dialled = nya_os_socket_connect(to, &client);
        nya_check(dialled == NYA_OS_SOCKET_OK || dialled == NYA_OS_SOCKET_WOULD_BLOCK, "expected %s", "dialled == NYA_OS_SOCKET_OK || dialled == NYA_OS_SOCKET_WOULD_BLOCK");
        defer nya_os_socket_close(client);

        NYA_OsSocket  accepted = NYA_OS_SOCKET_NONE;
        NYA_OsAddress from     = { 0 };
        nya_check(nya_os_socket_accept(listener, &accepted, &from) == NYA_OS_SOCKET_FAILED, "the first accept is refused");

        NYA_OsSocketWait watched = { .socket = listener, .readable = true };
        u32              ready   = 0;
        (void)nya_os_socket_wait(&watched, 1, WAIT_MS, &ready);
        nya_check(nya_os_socket_accept(listener, &accepted, &from) == NYA_OS_SOCKET_OK, "and the connection is still there for the next one");
        defer nya_os_socket_close(accepted);

        u64 sent = 0;
        nya_check(nya_os_socket_send(accepted, (const u8*)"0123456789", 10, &sent) == NYA_OS_SOCKET_OK, "a short send is not a failure");
        nya_check(sent >= 1 && sent < 10, "a short send takes part of it, got " FMTu64, sent);

        // the client side was dialled, which this plan does not follow, so its send is whole.
        u64 whole = 0;
        nya_check(nya_os_socket_send(client, (const u8*)"0123456789", 10, &whole) == NYA_OS_SOCKET_OK && whole == 10, "an unfollowed socket is not faulted");

        plan.rules[NYA_OS_FAULT_SOCKET_RECEIVE] = (NYA_OsFaultRule){ .fail_call = plan.calls[NYA_OS_FAULT_SOCKET_RECEIVE] + 1, .fail_kind = NYA_OS_FAULT_RESET };

        u8  buffer[16];
        u64 read = 0;
        nya_check(nya_os_socket_receive(accepted, buffer, sizeof(buffer), &read) == NYA_OS_SOCKET_CLOSED, "a reset reads as the connection gone");

        // the peer sees it too: its next read past what was sent is the end, or a reset.
        (void)wait_readable(client);
        u8                 drained[16];
        u64                drained_size = 0;
        NYA_OsSocketStatus peer         = NYA_OS_SOCKET_OK;
        for (u32 i = 0; i < 4 && peer == NYA_OS_SOCKET_OK; i++) peer = nya_os_socket_receive(client, drained, sizeof(drained), &drained_size);
        nya_check(peer == NYA_OS_SOCKET_CLOSED, "the peer finds the stream ended, got %u", (u32)peer);

        nya_os_fault_plan_detach();
    }

    // TEST: a page commit can be refused, and detached the same commit works.
    {
        u64 page    = nya_os_page_size();
        u8* address = nya_os_page_reserve(page);
        nya_check(address != nullptr, "expected %s", "address != nullptr");
        defer (void)nya_os_page_release(address, page);

        NYA_OsFaultPlan plan = { .seed = 13 };
        plan.rules[NYA_OS_FAULT_PAGE_COMMIT] = (NYA_OsFaultRule){ .fail_call = 1, .fail_kind = NYA_OS_FAULT_FAIL };

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");
        nya_check(!nya_os_page_commit(address, page), "the commit is refused");
        nya_os_fault_plan_detach();

        nya_check(nya_os_page_commit(address, page), "and without a plan it goes through");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
