/**
 * Roles, ranks, overwrites and the resolver.
 *
 * Two of these cases are laws rather than examples, and they are the ones that matter: resolution
 * against a naive oracle written straight from the six steps in the header, and "no sequence of
 * operations gives anyone a permission the actor did not hold", which is the whole point of the
 * hierarchy rules.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** A program's own permissions, which is what every bit below the reserved three is for. */
#define TEST_KICK   (1ULL << 0)
#define TEST_INVITE (1ULL << 1)
#define TEST_SPEAK  (1ULL << 2)
#define TEST_BUILD  (1ULL << 3)
#define TEST_ALL    (TEST_KICK | TEST_INVITE | TEST_SPEAK | TEST_BUILD)

#define ALICE 1001ULL
#define BOB   1002ULL
#define CAROL 1003ULL

#define HALL 5001ULL
#define VAULT 5002ULL

/** A uniform draw in [min, max], which is the only shape these laws need. */
static u32 pick(NYA_RNG* rng, u32 min, u32 max) {
    return nya_rng_sample_u32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = min, .max = max } });
}

/** A draw over the bits a test permission set may hold. */
static NYA_Permission pick_permissions(NYA_RNG* rng, NYA_Permission mask) {
    return (NYA_Permission)nya_rng_sample_u64(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } }) &
           mask;
}

/**
 * The six steps of the header, written again as plainly as possible.
 *
 * Deliberately not sharing a line of code with the implementation: an oracle that called the same
 * helpers would agree with it about a mistake they both made.
 */
static NYA_Permission oracle_resolve(const NYA_Permissions* permissions, u64 subject, u64 resource) {
    if (subject == NYA_PERMISSION_SYSTEM || (nya_permissions_owner(permissions) != NYA_PERMISSION_SYSTEM && subject == nya_permissions_owner(permissions))) {
        return ~0ULL;
    }

    u64            roles = nya_permission_subject_roles(permissions, subject);
    NYA_Permission base  = 0;

    for (u32 role = 0; role < nya_permission_role_count(permissions); role++) {
        if ((roles & (1ULL << role)) != 0) base |= nya_permission_role_allows(permissions, role);
    }

    if ((base & NYA_PERMISSION_ADMINISTRATOR) != 0) return ~0ULL;

    NYA_Permission allow = 0;
    NYA_Permission deny  = 0;

    if (nya_permission_overwrite_get(permissions, resource, NYA_PERMISSION_TARGET_ROLE, NYA_PERMISSION_ROLE_EVERYONE, &allow, &deny)) {
        base &= ~deny;
        base |= allow;
    }

    NYA_Permission role_allow = 0;
    NYA_Permission role_deny  = 0;

    for (u32 role = 1; role < nya_permission_role_count(permissions); role++) {
        if ((roles & (1ULL << role)) == 0) continue;
        if (!nya_permission_overwrite_get(permissions, resource, NYA_PERMISSION_TARGET_ROLE, role, &allow, &deny)) continue;

        role_allow |= allow;
        role_deny  |= deny;
    }

    base &= ~role_deny;
    base |= role_allow;

    if (nya_permission_overwrite_get(permissions, resource, NYA_PERMISSION_TARGET_SUBJECT, subject, &allow, &deny)) {
        base &= ~deny;
        base |= allow;
    }

    return base;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_permission");
    defer      nya_arena_destroy(arena);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a fresh table denies everything, to everyone.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);
        nya_check(guild != nullptr, "a table is made");

        nya_check(nya_permission_role_count(guild) == 1, "@everyone and nothing else, got %u", nya_permission_role_count(guild));
        nya_check(nya_permission_subject_count(guild) == 0, "and nobody in it");

        nya_check(nya_permission_resolve(guild, ALICE, HALL) == NYA_PERMISSION_NONE, "a stranger may do nothing");
        nya_check(!nya_permission_has(guild, ALICE, HALL, TEST_SPEAK), "including speak");

        // a subject the table has never seen is not an error, it is a subject holding @everyone alone.
        nya_check(nya_permission_subject_roles(guild, ALICE) == 1ULL, "everyone holds @everyone");
        nya_check(nya_permission_subject_rank(guild, ALICE) == 0, "at rank zero");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: roles union, and @everyone reaches a subject that was never added.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        nya_check(nya_permission_role_edit(guild, NYA_PERMISSION_SYSTEM, NYA_PERMISSION_ROLE_EVERYONE, 0, TEST_SPEAK, 100).ok, "@everyone may speak");
        nya_check(nya_permission_has(guild, ALICE, HALL, TEST_SPEAK), "which reaches a subject nobody added");

        u32 builder = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "builder", 10, TEST_BUILD, 100, &builder).ok, "a role is added");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, builder, 100).ok, "and granted");

        nya_check(nya_permission_resolve(guild, ALICE, HALL) == (TEST_SPEAK | TEST_BUILD), "the union of what she holds");
        nya_check(nya_permission_subject_rank(guild, ALICE) == 10, "and her rank is her highest role");
        nya_check(nya_permission_resolve(guild, BOB, HALL) == TEST_SPEAK, "which bob does not hold");

        nya_check(nya_permission_role_revoke(guild, NYA_PERMISSION_SYSTEM, ALICE, builder, 100).ok, "revoked again");
        nya_check(nya_permission_resolve(guild, ALICE, HALL) == TEST_SPEAK, "and the permission goes with it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the six steps, in the order the header states.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        u32 member = 0;
        u32 guard  = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "member", 5, TEST_SPEAK | TEST_INVITE, 100, &member).ok, "member");
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "guard", 20, TEST_KICK, 100, &guard).ok, "guard");

        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, member, 100).ok, "alice is a member");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, guard, 100).ok, "and a guard");

        // @everyone is denied speaking in the vault, so alice loses it there and keeps it in the hall.
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_ROLE, NYA_PERMISSION_ROLE_EVERYONE, 0,
                                               TEST_SPEAK, 100)
                      .ok,
                  "the vault is quiet");

        nya_check(nya_permission_has(guild, ALICE, HALL, TEST_SPEAK), "she speaks in the hall");
        nya_check(!nya_permission_has(guild, ALICE, VAULT, TEST_SPEAK), "and not in the vault");

        // a role allow beats the @everyone deny, because step 4 comes after step 3.
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_ROLE, guard, TEST_SPEAK, 0, 100).ok,
                  "guards may speak there");
        nya_check(nya_permission_has(guild, ALICE, VAULT, TEST_SPEAK), "so she speaks in the vault again");

        // and her own deny beats her role's allow, because step 5 comes after step 4.
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_SUBJECT, ALICE, 0, TEST_SPEAK, 100).ok,
                  "except for her");
        nya_check(!nya_permission_has(guild, ALICE, VAULT, TEST_SPEAK), "and she is quiet again");

        // one role denying what another allows: taken together the allow wins, so adding a role never
        // takes a permission away.
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_ROLE, member, 0, TEST_KICK, 100).ok,
                  "members may not kick in the vault");
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_ROLE, guard, TEST_SPEAK | TEST_KICK, 0, 100)
                      .ok,
                  "but guards may");
        nya_check(nya_permission_has(guild, ALICE, VAULT, TEST_KICK), "and she is both, so she may");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: administrator and the owner are the two short circuits.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        u32 admin = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "admin", 50, NYA_PERMISSION_ADMINISTRATOR, 100, &admin).ok, "an admin role");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, admin, 100).ok, "alice has it");

        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, VAULT, NYA_PERMISSION_TARGET_SUBJECT, ALICE, 0, TEST_ALL, 100).ok,
                  "and is denied everything in the vault");

        nya_check(nya_permission_has(guild, ALICE, VAULT, TEST_ALL), "which an administrator ignores");

        nya_check(nya_permissions_owner_set(guild, NYA_PERMISSION_SYSTEM, BOB, 100).ok, "bob owns the guild");
        nya_check(nya_permission_has(guild, BOB, VAULT, TEST_ALL | NYA_PERMISSION_RESERVED), "and may do anything anywhere");
        nya_check(nya_permission_outranks(guild, BOB, ALICE), "including outranking an administrator");
        nya_check(!nya_permission_outranks(guild, ALICE, BOB), "who cannot outrank him");

        // and the owner is not handed around: only the owner or the program itself may set it.
        nya_check(!nya_permissions_owner_set(guild, ALICE, ALICE, 100).ok, "an administrator cannot take the guild");
        nya_check(nya_permissions_owner(guild) == BOB, "and did not");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the hierarchy refuses every way around it.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        u32 officer = 0;
        u32 member  = 0;
        u32 elder   = 0;

        // she holds speak and kick, and not build: what she may hand out is exactly that, which is what
        // the refusals below are about.
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "officer", 20, NYA_PERMISSION_MANAGE_ROLES | TEST_KICK | TEST_SPEAK, 100,
                                          &officer)
                      .ok,
                  "officer");
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "member", 5, TEST_SPEAK, 100, &member).ok, "member");
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "elder", 40, TEST_BUILD, 100, &elder).ok, "elder");

        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, officer, 100).ok, "alice is an officer");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, CAROL, officer, 100).ok, "so is carol");

        // below her rank and within what she holds: allowed.
        nya_check(nya_permission_role_grant(guild, ALICE, BOB, member, 100).ok, "she may make bob a member");

        // a role that allows what she does not hold is refused even below her rank, which is the same
        // rule Discord has: managing roles is not a way to hand out a permission you were never given.
        u32 mason = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "mason", 10, TEST_BUILD, 100, &mason).ok, "a building role exists");
        nya_check(nya_permission_role_grant(guild, ALICE, BOB, mason, 100).kind == NYA_ERROR_PERMISSION_DENIED, "and she cannot hand it out");

        // above her rank: refused, however much she manages roles.
        nya_check(nya_permission_role_grant(guild, ALICE, BOB, elder, 100).kind == NYA_ERROR_PERMISSION_DENIED, "she cannot make him an elder");

        // equal rank: refused, which is what stops officers from arming each other.
        nya_check(nya_permission_role_grant(guild, ALICE, CAROL, member, 100).kind == NYA_ERROR_PERMISSION_DENIED, "nor touch a fellow officer");
        nya_check(nya_permission_role_revoke(guild, ALICE, CAROL, officer, 100).kind == NYA_ERROR_PERMISSION_DENIED, "nor demote one");

        // a role allowing what she does not hold: refused, so managing roles is not a way to gain any.
        u32 unused = 0;
        nya_check(nya_permission_role_add(guild, ALICE, "smith", 10, TEST_BUILD, 100, &unused).kind == NYA_ERROR_PERMISSION_DENIED,
                  "she cannot mint a role that builds");
        nya_check(nya_permission_role_add(guild, ALICE, "usher", 10, TEST_KICK, 100, &unused).ok, "but may mint one that kicks");

        // and an overwrite is not the way around it either.
        nya_check(nya_permission_overwrite_set(guild, ALICE, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, TEST_BUILD, 0, 100).kind ==
                      NYA_ERROR_PERMISSION_DENIED,
                  "nor hand out building through an overwrite");
        nya_check(nya_permission_overwrite_set(guild, ALICE, HALL, NYA_PERMISSION_TARGET_SUBJECT, CAROL, TEST_KICK, 0, 100).kind ==
                      NYA_ERROR_PERMISSION_DENIED,
                  "nor write one about her equal");
        nya_check(nya_permission_overwrite_set(guild, ALICE, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, TEST_KICK, 0, 100).ok,
                  "but may write one she holds, about someone below her");

        // without MANAGE_ROLES nothing at all.
        nya_check(nya_permission_role_grant(guild, BOB, BOB, member, 100).kind == NYA_ERROR_PERMISSION_DENIED, "a member grants nothing");
        nya_check(!nya_permission_outranks(guild, BOB, BOB), "and nobody outranks themselves");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the audit says who did what, and admits what it dropped.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        u32 role = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "member", 5, TEST_SPEAK, 700, &role).ok, "a role");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, role, 800).ok, "granted");

        nya_check(nya_permission_audit_count(guild) == 2, "two entries, got %u", nya_permission_audit_count(guild));

        NYA_PermissionAudit entry = { 0 };
        nya_check(nya_permission_audit_at(guild, 0, &entry), "the first is there");
        nya_check(entry.change == NYA_PERMISSION_CHANGE_ROLE_ADDED && entry.after_allow == TEST_SPEAK && entry.at_s == 700, "and says what it was");

        nya_check(nya_permission_audit_at(guild, 1, &entry), "the second too");
        nya_check(entry.change == NYA_PERMISSION_CHANGE_ROLE_GRANTED && entry.subject == ALICE && entry.role == role, "and who it was about");
        nya_check(!nya_permission_audit_at(guild, 2, &entry), "and there is no third");

        // past the ring, the oldest goes and the count says so out loud.
        for (u32 index = 0; index < NYA_PERMISSION_MAX_AUDIT + 10; index++) {
            nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, TEST_SPEAK, 0, 900).ok,
                      "an overwrite is written");
        }

        nya_check(nya_permission_audit_count(guild) == NYA_PERMISSION_MAX_AUDIT, "the ring is full, got %u", nya_permission_audit_count(guild));
        nya_check(nya_permission_audit_dropped(guild) == 12, "and dropped twelve, got %llu", (unsigned long long)nya_permission_audit_dropped(guild));

        nya_check(nya_permission_audit_at(guild, 0, &entry) && entry.change == NYA_PERMISSION_CHANGE_OVERWRITE_SET, "the oldest kept is the newest kind");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // LAW: resolution agrees with a naive reading of the six steps, always.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_RNG rng = nya_rng_create(.seed = "9E3779B97F4A7C15");

        for (u32 round = 0; round < 400; round++) {
            NYA_Arena* scratch = nya_arena_create(.name = "permission_law");
            defer      nya_arena_destroy(scratch);

            NYA_Permissions* table = nya_permissions_create(scratch);

            u32 roles = 1 + pick(&rng, 0, 6);
            for (u32 index = 1; index < roles; index++) {
                u32 role = 0;
                char name[NYA_PERMISSION_MAX_ROLE_NAME] = { 0 };
                (void)snprintf(name, sizeof(name), "role%u", index);

                (void)nya_permission_role_add(table, NYA_PERMISSION_SYSTEM, name, (u16)(1 + pick(&rng, 0, 40)),
                                              pick_permissions(&rng, TEST_ALL), 0, &role);
            }

            u64 subjects[4] = { ALICE, BOB, CAROL, 1004ULL };

            for (u32 index = 0; index < nya_carray_length(subjects); index++) {
                for (u32 role = 1; role < nya_permission_role_count(table); role++) {
                    if (pick(&rng, 0, 3) != 0) continue;

                    (void)nya_permission_role_grant(table, NYA_PERMISSION_SYSTEM, subjects[index], role, 0);
                }
            }

            u64 resources[3] = { HALL, VAULT, 0 };

            for (u32 index = 0; index < 8; index++) {
                u64 resource = resources[pick(&rng, 0, 2)];
                NYA_Permission allow = pick_permissions(&rng, TEST_ALL);
                NYA_Permission deny  = pick_permissions(&rng, TEST_ALL) & ~allow;

                if (pick(&rng, 0, 1) == 0) {
                    u32 role = pick(&rng, 0, nya_permission_role_count(table) - 1);
                    (void)nya_permission_overwrite_set(table, NYA_PERMISSION_SYSTEM, resource, NYA_PERMISSION_TARGET_ROLE, role, allow, deny, 0);
                } else {
                    u64 subject = subjects[pick(&rng, 0, 3)];
                    (void)nya_permission_overwrite_set(table, NYA_PERMISSION_SYSTEM, resource, NYA_PERMISSION_TARGET_SUBJECT, subject, allow, deny, 0);
                }
            }

            for (u32 index = 0; index < nya_carray_length(subjects); index++) {
                for (u32 which = 0; which < nya_carray_length(resources); which++) {
                    NYA_Permission resolved = nya_permission_resolve(table, subjects[index], resources[which]);
                    NYA_Permission expected = oracle_resolve(table, subjects[index], resources[which]);

                    nya_check(resolved == expected, "round %u: resolved %llx where the six steps give %llx", round, (unsigned long long)resolved,
                              (unsigned long long)expected);
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // LAW: no sequence of operations gives anyone what its actor did not hold.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_RNG rng = nya_rng_create(.seed = "D1B54A32D192ED03");

        for (u32 round = 0; round < 200; round++) {
            NYA_Arena* scratch = nya_arena_create(.name = "permission_escalation");
            defer      nya_arena_destroy(scratch);

            NYA_Permissions* table = nya_permissions_create(scratch);

            // one actor who manages roles and holds exactly one of the program's permissions, which is
            // the interesting shape: everything they can reach has to stay inside that one bit.
            u32 officer = 0;
            (void)nya_permission_role_add(table, NYA_PERMISSION_SYSTEM, "officer", 20, NYA_PERMISSION_MANAGE_ROLES | TEST_KICK, 0, &officer);
            (void)nya_permission_role_grant(table, NYA_PERMISSION_SYSTEM, ALICE, officer, 0);

            NYA_Permission actor_holds = nya_permission_resolve(table, ALICE, 0);

            for (u32 step = 0; step < 24; step++) {
                u64            subject  = 1000ULL + pick(&rng, 1, 4);
                u64            resource = pick(&rng, 0, 1) == 0 ? HALL : VAULT;
                NYA_Permission wanted   = pick_permissions(&rng, TEST_ALL | NYA_PERMISSION_RESERVED);

                switch (pick(&rng, 0, 3)) {
                    case 0: {
                        u32  role                               = 0;
                        char name[NYA_PERMISSION_MAX_ROLE_NAME] = { 0 };
                        (void)snprintf(name, sizeof(name), "r%u_%u", round, step);

                        (void)nya_permission_role_add(table, ALICE, name, (u16)pick(&rng, 1, 60), wanted, 0, &role);
                        break;
                    }
                    case 1: {
                        u32 role = pick(&rng, 0, nya_permission_role_count(table) - 1);
                        (void)nya_permission_role_grant(table, ALICE, subject, role, 0);
                        break;
                    }
                    case 2: {
                        (void)nya_permission_overwrite_set(table, ALICE, resource, NYA_PERMISSION_TARGET_SUBJECT, subject, wanted, 0, 0);
                        break;
                    }
                    default: {
                        u32 role = pick(&rng, 0, nya_permission_role_count(table) - 1);
                        (void)nya_permission_overwrite_set(table, ALICE, resource, NYA_PERMISSION_TARGET_ROLE, role, wanted, 0, 0);
                        break;
                    }
                }
            }

            // nothing the actor did may have handed anyone a bit the actor did not hold, on any resource.
            for (u32 index = 1; index <= 4; index++) {
                u64 subject = 1000ULL + index;

                if (subject == ALICE) continue;

                NYA_Permission in_hall  = nya_permission_resolve(table, subject, HALL);
                NYA_Permission in_vault = nya_permission_resolve(table, subject, VAULT);

                nya_check((in_hall & ~actor_holds) == 0, "round %u: the hall gave away %llx", round, (unsigned long long)(in_hall & ~actor_holds));
                nya_check((in_vault & ~actor_holds) == 0, "round %u: the vault gave away %llx", round, (unsigned long long)(in_vault & ~actor_holds));
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a table is edited while it runs: labels, removal, and clearing.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Permissions* guild = nya_permissions_create(arena);

        // the engine's three are named already, so an editor has rows before a program says anything.
        nya_check(strcmp(nya_permission_label(guild, NYA_PERMISSION_ADMINISTRATOR), "administrator") == 0, "the reserved bits are named");
        nya_check(nya_permission_label(guild, TEST_KICK)[0] == '\0', "and a program's are not, until it says so");

        nya_check(nya_permission_label_set(guild, TEST_KICK, "kick").ok, "a program names its own");
        nya_check(strcmp(nya_permission_label(guild, TEST_KICK), "kick") == 0, "which reads back");
        nya_check((nya_permission_labelled(guild) & TEST_KICK) != 0, "and joins the vocabulary an editor walks");
        nya_check((nya_permission_labelled(guild) & TEST_BUILD) == 0, "while an unnamed bit does not");

        // a label names one bit, because a row in an editor toggles one thing.
        nya_check(!nya_permission_label_set(guild, TEST_KICK | TEST_BUILD, "both").ok, "a set cannot be labelled");
        nya_check(!nya_permission_label_set(guild, 0, "nothing").ok, "nor can nothing");

        u32 officer = 0;
        nya_check(nya_permission_role_add(guild, NYA_PERMISSION_SYSTEM, "officer", 10, TEST_KICK, 100, &officer).ok, "a role to remove");
        nya_check(nya_permission_role_grant(guild, NYA_PERMISSION_SYSTEM, ALICE, officer, 100).ok, "granted to someone");
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, HALL, NYA_PERMISSION_TARGET_ROLE, officer, TEST_SPEAK, 0, 100).ok,
                  "with an overwrite about it");
        nya_check(nya_permission_overwrite_count(guild) == 1, "which is counted");

        nya_check(nya_permission_has(guild, ALICE, HALL, TEST_KICK | TEST_SPEAK), "she holds both through it");

        nya_check(nya_permission_role_remove(guild, NYA_PERMISSION_SYSTEM, officer, 100).ok, "the role is removed");
        nya_check(nya_permission_resolve(guild, ALICE, HALL) == NYA_PERMISSION_NONE, "and takes both with it");
        nya_check(nya_permission_overwrite_count(guild) == 0, "including the overwrite that named it");
        nya_check(!nya_permission_role_remove(guild, NYA_PERMISSION_SYSTEM, NYA_PERMISSION_ROLE_EVERYONE, 100).ok, "@everyone stays");

        // clearing one overwrite rather than the role it belonged to.
        nya_check(nya_permission_overwrite_set(guild, NYA_PERMISSION_SYSTEM, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, TEST_SPEAK, 0, 100).ok,
                  "an overwrite about bob");
        nya_check(nya_permission_has(guild, BOB, HALL, TEST_SPEAK), "which reaches him");
        nya_check(nya_permission_overwrite_clear(guild, NYA_PERMISSION_SYSTEM, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, 100).ok, "cleared again");
        nya_check(!nya_permission_has(guild, BOB, HALL, TEST_SPEAK), "and no longer reaches him");
        nya_check(nya_permission_overwrite_clear(guild, NYA_PERMISSION_SYSTEM, HALL, NYA_PERMISSION_TARGET_SUBJECT, BOB, 100).ok,
                  "clearing what is not there is the end state the caller asked for");

        // and emptying the table leaves what a new one would be.
        nya_check(nya_permissions_owner_set(guild, NYA_PERMISSION_SYSTEM, ALICE, 100).ok, "an owner");
        nya_permissions_destroy(guild);

        nya_check(nya_permission_role_count(guild) == 1 && nya_permission_subject_count(guild) == 0, "emptied back to @everyone");
        nya_check(nya_permissions_owner(guild) == NYA_PERMISSION_SYSTEM, "with no owner");
        nya_check(nya_permission_audit_count(guild) == 0, "and no history of a session that ended");
        nya_check(nya_permission_resolve(guild, ALICE, HALL) == NYA_PERMISSION_NONE, "and nobody holding anything");
    }

    printf("PASSED: permission\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
