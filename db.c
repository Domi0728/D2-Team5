#include "app.h"
#include "schema.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3 *db;
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
int db_failed;
const char *test_clock_path;

static char *format_sql(const char *fmt, va_list ap) {
    char *sql = sqlite3_vmprintf(fmt, ap);
    if (!sql) abort();
    return sql;
}

void execute(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *sql = format_sql(fmt, ap);
    va_end(ap);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) db_failed = 1;
    sqlite3_free(sql);
}

static J *read_query(char *sql) {
    J *result = arr();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        db_failed = 1;
    } else {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            J *row = obj();
            for (int i = 0; i < sqlite3_column_count(st); i++) {
                J *v = NULL;
                int type = sqlite3_column_type(st, i);
                if (type == SQLITE_INTEGER) v = json_object_new_int64(sqlite3_column_int64(st, i));
                else if (type == SQLITE_FLOAT) v = json_object_new_double(sqlite3_column_double(st, i));
                else if (type != SQLITE_NULL) v = json_object_new_string((const char *)sqlite3_column_text(st, i));
                json_object_object_add(row, sqlite3_column_name(st, i), v);
            }
            json_object_array_add(result, row);
        }
        if (rc != SQLITE_DONE) db_failed = 1;
    }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    return result;
}

J *query(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *sql = format_sql(fmt, ap);
    va_end(ap);
    return read_query(sql);
}

J *first(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *sql = format_sql(fmt, ap);
    va_end(ap);
    J *a = read_query(sql);
    J *row = json_object_array_length(a) ? json_object_get(json_object_array_get_idx(a, 0)) : NULL;
    json_object_put(a);
    return row;
}

int64_t scalar(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *sql = format_sql(fmt, ap);
    va_end(ap);
    sqlite3_stmt *st = NULL;
    int64_t value = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) value = sqlite3_column_int64(st, 0);
        else if (rc != SQLITE_DONE) db_failed = 1;
    } else db_failed = 1;
    sqlite3_finalize(st);
    sqlite3_free(sql);
    return value;
}

int cfg(const char *key) {
    return (int)scalar("SELECT value FROM config WHERE key=%Q", key);
}

int64_t now_seconds(void) {
    if (test_clock_path) {
        FILE *f = fopen(test_clock_path, "r");
        long long value = 0;
        if (f) {
            int read_ok = fscanf(f, "%lld", &value);
            fclose(f);
            if (read_ok == 1 && value > 0) return value;
        }
    }
    return (int64_t)time(NULL);
}

void db_init(const char *path) {
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database\n");
        exit(1);
    }
    sqlite3_busy_timeout(db, 15000);
    execute("PRAGMA foreign_keys=ON");
    execute("PRAGMA journal_mode=WAL");
    for (size_t i=0;i<sizeof SCHEMA/sizeof SCHEMA[0];i++) execute("%s", SCHEMA[i]);
    execute("INSERT OR IGNORE INTO config VALUES"
        "('default_tolerance_minutes',15),('max_tolerance_minutes',120),"
        "('invite_minutes',5),('max_capacity',6),('late_cancel_minutes',10),"
        "('overlap_minutes',60),('start_grace_minutes',30),('retention_days',90)");
    if (db_failed) { fprintf(stderr, "Database initialization failed\n"); exit(1); }
}
