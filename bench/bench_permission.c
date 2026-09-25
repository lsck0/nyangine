/**
 * Permission resolution: the role → allowed? question a route or a tick asks per request.
 *
 * This is the hot path the header promises is "a handful of ORs over a fixed table rather than a
 * query", so the measurement is throughput: how many checks a core clears in a second against a
 * table shaped like a real guild — several roles, a couple of hundred subjects holding a mix of
 * them, and a few rooms carrying overwrites.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/* A program's own permissions live below the reserved three. A guild-sized set of them. */
#define P_SPEAK   (1ULL << 0)
#define P_KICK    (1ULL << 1)
#define P_INVITE  (1ULL << 2)
#define P_BUILD   (1ULL << 3)
#define P_PIN     (1ULL << 4)
#define P_MANAGE  (1ULL << 5)

#define SUBJECTS  200U
#define RESOURCES 8U

/** The first subject id; kept clear of NYA_PERMISSION_SYSTEM, which answers yes to everything. */
#define FIRST_SUBJECT 1000ULL

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_permission");
    defer      nya_arena_destroy(arena);

    NYA_Permissions* table = nya_permissions_create(arena);

    /* Built as NYA_PERMISSION_SYSTEM, the actor that outranks everyone and holds every bit, so the setup is not itself a test of the hierarchy rules. What is measured below is resolution, not the gated writes that fill the table. */
    const u64 sys = NYA_PERMISSION_SYSTEM;

    u32 member = 0;
    u32 mod    = 0;
    u32 admin  = 0;

    NYA_EXPECT(nya_permission_role_add(table, sys, "member", 10, P_SPEAK | P_INVITE, 0, &member));
    NYA_EXPECT(nya_permission_role_add(table, sys, "moderator", 20, P_SPEAK | P_INVITE | P_KICK | P_PIN, 0, &mod));
    NYA_EXPECT(nya_permission_role_add(table, sys, "admin", 30, P_SPEAK | P_INVITE | P_KICK | P_PIN | P_BUILD | P_MANAGE, 0, &admin));

    /* Every subject holds member; a third also moderate and a tenth administer, which is roughly the shape of a real membership and enough held roles that the union in step one is not trivial. */
    for (u32 s = 0; s < SUBJECTS; s++) {
        u64 subject = FIRST_SUBJECT + s;

        NYA_EXPECT(nya_permission_role_grant(table, sys, subject, member, 0));
        if (s % 3 == 0) NYA_EXPECT(nya_permission_role_grant(table, sys, subject, mod, 0));
        if (s % 10 == 0) NYA_EXPECT(nya_permission_role_grant(table, sys, subject, admin, 0));
    }

    /*
     * A few rooms that differ from the default: one denies speaking to @everyone, one hands the
     * moderator role a pin allow, so resolution has overwrites to fold in rather than roles alone.
     */
    for (u32 r = 1; r < RESOURCES; r++) {
        u64 resource = 5000ULL + r;

        if (r % 2 == 0) {
            NYA_EXPECT(nya_permission_overwrite_set(table, sys, resource, NYA_PERMISSION_TARGET_ROLE, NYA_PERMISSION_ROLE_EVERYONE, 0, P_SPEAK, 0));
        } else {
            NYA_EXPECT(nya_permission_overwrite_set(table, sys, resource, NYA_PERMISSION_TARGET_ROLE, mod, P_PIN, 0, 0));
        }
    }

    nya_bench_begin("permission resolution (the per-request hot path)");

    // The one call a route makes: does this subject hold this bit on this room. Swept across every subject and every room so the sample is not one lucky cache line.
    nya_bench("has, subject x resource", SUBJECTS * RESOURCES, {
        u32 granted = 0;
        for (u32 s = 0; s < SUBJECTS; s++) {
            for (u32 r = 0; r < RESOURCES; r++) {
                granted += nya_permission_has(table, FIRST_SUBJECT + s, 5000ULL + r, P_KICK) ? 1U : 0U;
            }
        }
        nya_bench_keep(granted);
    });

    // The whole resolved set, which a screen that greys out what a subject cannot do asks for once and then reads bit by bit. Same sweep, so the two numbers sit beside each other.
    nya_bench("resolve, subject x resource", SUBJECTS * RESOURCES, {
        NYA_Permission mixed = 0;
        for (u32 s = 0; s < SUBJECTS; s++) {
            for (u32 r = 0; r < RESOURCES; r++) {
                mixed |= nya_permission_resolve(table, FIRST_SUBJECT + s, 5000ULL + r);
            }
        }
        nya_bench_keep(mixed);
    });

    // Resource zero: no room in particular, which resolves the held roles alone and skips every overwrite lookup. The floor a guild-wide check pays.
    nya_bench("has, roles alone (no resource)", SUBJECTS, {
        u32 granted = 0;
        for (u32 s = 0; s < SUBJECTS; s++) granted += nya_permission_has(table, FIRST_SUBJECT + s, 0, P_INVITE) ? 1U : 0U;
        nya_bench_keep(granted);
    });

    return nya_bench_end();
}
