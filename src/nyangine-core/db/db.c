// db_extensions.c is absent: the sqlean bundle entry point, compiled on its own by the vendor rule. See build/vendor/vendor_sqlean.h.
#include "nyangine-core/db/db_sql.c"
// After db_sql.c: shares that file's NYA_Database and SQLite error mapping in one translation unit.
#include "nyangine-core/db/db_backup.c"
// After db_sql.c, whose connection and bound values both are written in terms of.
#include "nyangine-core/db/db_orm.c"
// After db_orm.c, whose statement builder, name parser and table reader it derives a plan with.
#include "nyangine-core/db/db_migrate.c"
// After db_sql.c: a content-addressed store keyed by the SHA-256 of an object's bytes.
#include "nyangine-core/db/db_blob.c"
// After db_sql.c: a persistent job queue, one SQLite table with retries, backoff, deadlines and unique keys.
#include "nyangine-core/db/db_jobs.c"
// After db_sql.c and db_jobs.c: opens a connection per worker and drives claim/complete/fail to clone a queue per worker.
#include "nyangine-core/db/db_jobworker.c"
