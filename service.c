#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int is_admin(Request *r) { return !strcmp(s(r->user,"role"),"ADMIN"); }
int eligible(J *u,int64_t now) {
    return u && !strcmp(s(u,"role"),"STUDENT") && !strcmp(s(u,"verify_status"),"APPROVED")
        && !n(u,"suspended") && n(u,"restriction_until")<=now;
}
int active_member(int64_t ride,int64_t uid) {
    return scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND user_id=%lld AND active=1",LL(ride),LL(uid))>0;
}
int member_count(int64_t ride) {
    return (int)scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND active=1",LL(ride));
}
int overlaps(int64_t uid,int64_t departure,int64_t exclude) {
    return scalar("SELECT count(*) FROM memberships m JOIN rides r ON r.id=m.ride_id "
        "WHERE m.user_id=%lld AND m.active=1 AND r.id<>%lld "
        "AND r.status IN ('WAITING','CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS') "
        "AND abs(r.departure_at-%lld)<%d",LL(uid),LL(exclude),LL(departure),cfg("overlap_minutes")*60)>0;
}
int version_matches(Request *r,J *ride) {
    return int_field(r->input,"expected_version",1,INT32_MAX) && n(r->input,"expected_version")==n(ride,"version");
}
void audit(Request *r,const char *action,int64_t entity,const char *details) {
    execute("INSERT INTO audit_logs(actor_id,action,entity_id,details,created_at) VALUES(%lld,%Q,%lld,%Q,%lld)",
        LL(n(r->user,"id")),action,LL(entity),details,LL(r->now));
}
void notify(int64_t uid,int64_t ride,const char *key,const char *content,int64_t now) {
    if(ride) execute("INSERT OR IGNORE INTO notifications(user_id,ride_id,event_key,content,created_at) VALUES(%lld,%lld,%Q,%Q,%lld)",LL(uid),LL(ride),key,content,LL(now));
    else execute("INSERT OR IGNORE INTO notifications(user_id,event_key,content,created_at) VALUES(%lld,%Q,%Q,%lld)",LL(uid),key,content,LL(now));
}
void notify_members(int64_t ride,int64_t except,const char *key,const char *content,int64_t now) {
    J *members=query("SELECT user_id FROM memberships WHERE ride_id=%lld AND active=1 AND user_id<>%lld",LL(ride),LL(except));
    for(size_t i=0;i<json_object_array_length(members);i++) notify(n(json_object_array_get_idx(members,i),"user_id"),ride,key,content,now);
    json_object_put(members);
}
J *user_view(J *u,int private_view) {
    J *o=obj();jn(o,"id",n(u,"id"));js(o,"nickname",s(u,"nickname"));
    js(o,"avatar",s(u,"avatar"));js(o,"verify_status",s(u,"verify_status"));
    J *tags=json_tokener_parse(s(u,"tags"));json_object_object_add(o,"tags",tags?tags:arr());
    if(private_view) {
        js(o,"student_id",s(u,"student_id"));js(o,"real_name",s(u,"real_name"));js(o,"role",s(u,"role"));
        js(o,"phone",s(u,"phone"));jn(o,"contact_share",n(u,"contact_share"));jn(o,"history_visible",n(u,"history_visible"));
        jn(o,"suspended",n(u,"suspended"));jn(o,"restriction_until",n(u,"restriction_until"));
    }
    return o;
}
J *ride_view(int64_t id,Request *r) {
    J *o=first("SELECT r.*,u.nickname AS organiser_nickname FROM rides r JOIN users u ON u.id=r.organiser_id WHERE r.id=%lld",LL(id));
    if(!o)return error(r,404,"NOT_FOUND","Ride not found");
    int count=member_count(id);
    jn(o,"member_count",count);jn(o,"available_seats",n(o,"capacity")-count);
    J *tags=json_tokener_parse(s(o,"tags"));json_object_object_add(o,"tags",tags?tags:arr());
    if(is_admin(r) || active_member(id,n(r->user,"id"))) {
        J *members=query("SELECT m.user_id,u.nickname,m.member_type,m.consent_version,m.arrival_flag FROM memberships m "
            "JOIN users u ON u.id=m.user_id WHERE m.ride_id=%lld AND m.active=1 ORDER BY m.id",LL(id));
        json_object_object_add(o,"members",members);
    }
    return o;
}
void update_group(int64_t id,int64_t now) {
    int count=member_count(id);
    int pending=(int)scalar("SELECT count(*) FROM memberships m JOIN rides r ON r.id=m.ride_id "
        "WHERE r.id=%lld AND m.active=1 AND m.consent_version<>r.consent_version",LL(id));
    const char *state=count<2?"WAITING":pending?"RECONFIRMATION_REQUIRED":"CONFIRMED";
    execute("UPDATE rides SET status=%Q,updated_at=%lld WHERE id=%lld",state,LL(now),LL(id));
}
void maintenance(int64_t now) {
    J *expired=query("SELECT * FROM invitations WHERE status='PENDING' AND deadline<=%lld",LL(now));
    for(size_t i=0;i<json_object_array_length(expired);i++) {
        J *v=json_object_array_get_idx(expired,i);char key[100];snprintf(key,sizeof key,"invite:%lld:expired",LL(n(v,"id")));
        notify(n(v,"recipient_id"),n(v,"ride_id"),key,"Invitation expired",now);
        notify(n(v,"sender_id"),n(v,"ride_id"),key,"Invitation expired",now);
    }
    json_object_put(expired);
    execute("UPDATE invitations SET status='EXPIRED' WHERE status='PENDING' AND deadline<=%lld",LL(now));
    J *rides=query("SELECT id FROM rides WHERE (status IN ('WAITING','RECONFIRMATION_REQUIRED') AND departure_at<=%lld) "
        "OR (status='CONFIRMED' AND departure_at+%d<=%lld)",LL(now),cfg("start_grace_minutes")*60,LL(now));
    for(size_t i=0;i<json_object_array_length(rides);i++) {
        int64_t id=n(json_object_array_get_idx(rides,i),"id");char key[100];snprintf(key,sizeof key,"ride:%lld:expired",LL(id));
        execute("UPDATE rides SET status='EXPIRED',closed_at=%lld,updated_at=%lld,version=version+1 WHERE id=%lld",LL(now),LL(now),LL(id));
        execute("UPDATE invitations SET status='EXPIRED' WHERE ride_id=%lld AND status='PENDING'",LL(id));
        notify_members(id,0,key,"Ride expired",now);
    }
    json_object_put(rides);
    execute("DELETE FROM sessions WHERE expires_at<=%lld",LL(now));
    execute("DELETE FROM idempotency WHERE created_at<%lld",LL(now-86400));
    execute("DELETE FROM login_attempts WHERE window_start<%lld",LL(now-86400));
}

J *handle_request(Request *r) {
    r->status=200;r->now=now_seconds();
    if(!strcmp(r->path,"/health") && !strcmp(r->method,"GET"))return ok();
    pthread_mutex_lock(&db_mutex);
    db_failed=0;
    execute("BEGIN IMMEDIATE");maintenance(r->now);
    if(db_failed) { execute("ROLLBACK");pthread_mutex_unlock(&db_mutex);return error(r,503,"DATABASE_BUSY","Database unavailable; retry later"); }
    execute("COMMIT");
    J *result=NULL;
    int unauth=!strcmp(r->path,"/api/register") || !strcmp(r->path,"/api/login");
    execute("BEGIN IMMEDIATE");
    if(!unauth) {
        char hash[65];digest(r->token,hash);
        r->user=first("SELECT u.* FROM users u JOIN sessions s ON s.user_id=u.id WHERE s.token_hash=%Q AND s.expires_at>%lld",hash,LL(r->now));
        if(!r->user) {result=error(r,401,"UNAUTHENTICATED","Valid bearer token required");goto done;}
    }
    int write=strcmp(r->method,"GET")!=0;
    char fingerprint[65]={0};
    if(write && !unauth) {
        size_t len=strlen(r->key);
        if(len<8 || len>80 || strspn(r->key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")!=len) {
            result=error(r,400,"IDEMPOTENCY_KEY_REQUIRED","Use a unique Idempotency-Key header of 8 to 80 characters");goto done;
        }
        char *payload=sqlite3_mprintf("%s\n%s\n%s",r->method,r->path,r->body);digest(payload,fingerprint);sqlite3_free(payload);
        J *cached=first("SELECT * FROM idempotency WHERE user_id=%lld AND key=%Q",LL(n(r->user,"id")),r->key);
        if(cached) {
            if(strcmp(s(cached,"fingerprint"),fingerprint)) result=error(r,409,"KEY_REUSED","Use a new key for a different request");
            else result=json_tokener_parse(s(cached,"result"));
            json_object_put(cached);goto done;
        }
    }
    if(unauth) result=auth_routes(r);
    else {
        result=auth_routes(r);
        if(!result)result=ride_routes(r);
        if(!result)result=other_routes(r);
    }
    if(!result)result=error(r,404,"NOT_FOUND","Unknown endpoint or HTTP method");
    if(write && !unauth && r->status<400 && !db_failed)
        execute("INSERT INTO idempotency VALUES(%lld,%Q,%Q,%Q,%lld)",LL(n(r->user,"id")),r->key,fingerprint,json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN),LL(r->now));
done:
    if(db_failed) {json_object_put(result);result=error(r,503,"DATABASE_ERROR","Database operation failed; refresh and retry");}
    int failed=db_failed;
    if(r->status>=400 && !(unauth && !strcmp(r->path,"/api/login") && !failed)) execute("ROLLBACK");
    else execute("COMMIT");
    if(!failed && db_failed) {execute("ROLLBACK");json_object_put(result);result=error(r,503,"DATABASE_ERROR","Commit failed; refresh and retry");}
    json_object_put(r->user);r->user=NULL;
    pthread_mutex_unlock(&db_mutex);
    return result;
}
