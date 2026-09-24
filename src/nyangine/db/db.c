// db_extensions.c is deliberately absent: it is the sqlean bundle's entry point and is compiled on
// its own by the vendor rule, against sqlean's standard and warnings rather than the engine's. See
// build/vendor/vendor_sqlean.h.
#include "nyangine/db/db_sql.c"
// After db_sql.c: it reaches into that file's NYA_Database and its SQLite error mapping, the two
// being one translation unit.
#include "nyangine/db/db_backup.c"
// After db_sql.c, whose connection and bound values both are written in terms of.
#include "nyangine/db/db_orm.c"
// After db_orm.c, whose statement builder, name parser and table reader it derives a plan with.
#include "nyangine/db/db_migrate.c"
// After db_sql.c, whose connection and bound values it stores objects through, and above nothing: a
// content-addressed store keyed by the SHA-256 of an object's bytes.
#include "nyangine/db/db_blob.c"
