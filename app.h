#ifndef RIDE_APP_H
#define RIDE_APP_H
#define _GNU_SOURCE
#include <json-c/json.h>
#include <sqlite3.h>
#include <microhttpd.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef json_object J;
#define LL(x) ((long long)(x))
typedef struct {
    const char *method;
    const char *path;
    const char *body;
    const char *token;
    const char *key;
    struct MHD_Connection *connection;
    J *input;
    J *user;
    int status;
    int64_t now;
} Request;

extern sqlite3 *db;
extern pthread_mutex_t db_mutex;
extern int db_failed;
extern const char *allowed_origins;
extern const char *test_clock_path;

void db_init(const char *path);
void execute(const char *fmt, ...);
J *query(const char *fmt, ...);
J *first(const char *fmt, ...);
int64_t scalar(const char *fmt, ...);
int64_t now_seconds(void);
int cfg(const char *key);
J *obj(void);
J *arr(void);
const char *s(J *j, const char *key);
int64_t n(J *j, const char *key);
void js(J *j, const char *key, const char *value);
void jn(J *j, const char *key, int64_t value);
J *error(Request *r, int status, const char *code, const char *message);
J *ok(void);
int fields(Request *r, const char *allowed);
int text_field(J *j, const char *key, int min, int max);
int int_field(J *j, const char *key, int64_t min, int64_t max);
int bool_field(J *j, const char *key);
int strong_password(const char *password);
int password_hash(const char *password, char out[162]);
int password_verify(const char *password, const char *stored);
int random_hex(char *out, int bytes);
void digest(const char *input, char out[65]);
J *handle_request(Request *r);
J *auth_routes(Request *r);
J *ride_routes(Request *r);
J *other_routes(Request *r);
void maintenance(int64_t now);
void audit(Request *r, const char *action, int64_t entity, const char *details);
void notify(int64_t uid, int64_t ride, const char *key, const char *content, int64_t now);
void notify_members(int64_t ride, int64_t except_uid, const char *key, const char *content, int64_t now);
int eligible(J *u, int64_t now);
int is_admin(Request *r);
int version_matches(Request *r, J *ride);
int active_member(int64_t ride, int64_t uid);
int member_count(int64_t ride);
int overlaps(int64_t uid, int64_t departure, int64_t exclude_ride);
J *ride_view(int64_t id, Request *r);
J *user_view(J *u, int private_view);
void update_group(int64_t id, int64_t now);
const char *arg(Request *r, const char *name);
int64_t arg_number(Request *r, const char *name, int64_t fallback);
int path_id(const char *path, const char *prefix, const char *suffix, int64_t *id);
#endif
