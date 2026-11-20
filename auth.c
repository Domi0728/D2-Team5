#include "app.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

J *auth_routes(Request *r) {
    J *in=r->input;
    int post=!strcmp(r->method,"POST"),get=!strcmp(r->method,"GET");
    if(post && !strcmp(r->path,"/api/register")) {
        if(!fields(r,"|student_id|real_name|nickname|password|") || !text_field(in,"student_id",10,10) ||
            strspn(s(in,"student_id"),"0123456789")!=10 || !text_field(in,"real_name",1,160) ||
            !text_field(in,"nickname",1,80) || !text_field(in,"password",10,128) || !strong_password(s(in,"password")))
            return error(r,422,"INVALID_INPUT","Use a 10-digit student ID, names and a password with 10+ characters, letters and digits");
        if(scalar("SELECT count(*) FROM users WHERE student_id=%Q",s(in,"student_id")))return error(r,409,"DUPLICATE_ACCOUNT","Student ID already registered");
        char hash[162];if(!password_hash(s(in,"password"),hash))return error(r,500,"CRYPTO_ERROR","Unable to create account");
        execute("INSERT INTO users(student_id,real_name,nickname,password_hash,created_at) VALUES(%Q,%Q,%Q,%Q,%lld)",
            s(in,"student_id"),s(in,"real_name"),s(in,"nickname"),hash,LL(r->now));
        J *o=obj();jn(o,"id",sqlite3_last_insert_rowid(db));js(o,"verify_status","PENDING");return o;
    }
    if(post && !strcmp(r->path,"/api/login")) {
        if(!fields(r,"|student_id|password|") || !text_field(in,"student_id",1,80) || !text_field(in,"password",1,128))return error(r,422,"INVALID_INPUT","student_id and password required");
        char bucket[65];digest(s(in,"student_id"),bucket);
        int failures=(int)scalar("SELECT failures FROM login_attempts WHERE bucket=%Q AND window_start>%lld",bucket,LL(r->now-900));
        if(failures>=10)return error(r,429,"RATE_LIMITED","Too many attempts; try again after 15 minutes");
        J *u=first("SELECT * FROM users WHERE student_id=%Q",s(in,"student_id"));
        int valid=0;
        if(u)valid=password_verify(s(in,"password"),s(u,"password_hash"));
        else {char unused[162];password_hash(s(in,"password"),unused);}
        if(!valid) {
            execute("INSERT INTO login_attempts VALUES(%Q,1,%lld) ON CONFLICT(bucket) DO UPDATE SET "
                "failures=CASE WHEN window_start<=%lld THEN 1 ELSE failures+1 END,"
                "window_start=CASE WHEN window_start<=%lld THEN %lld ELSE window_start END",bucket,LL(r->now),LL(r->now-900),LL(r->now-900),LL(r->now));
            json_object_put(u);return error(r,401,"INVALID_CREDENTIALS","Invalid student ID or password");
        }
        execute("DELETE FROM login_attempts WHERE bucket=%Q",bucket);
        char token[65],hash[65];if(!random_hex(token,32)){json_object_put(u);return error(r,500,"CRYPTO_ERROR","Unable to create session");}
        digest(token,hash);execute("INSERT INTO sessions VALUES(%Q,%lld,%lld)",hash,LL(n(u,"id")),LL(r->now+28800));
        J *o=obj();js(o,"access_token",token);js(o,"token_type","bearer");jn(o,"expires_at",r->now+28800);
        json_object_object_add(o,"user",user_view(u,1));json_object_put(u);return o;
    }
    if(!r->user)return NULL;
    if(get && !strcmp(r->path,"/api/me"))return user_view(r->user,1);
    if(post && !strcmp(r->path,"/api/logout")) {
        char hash[65];digest(r->token,hash);execute("DELETE FROM sessions WHERE token_hash=%Q",hash);return ok();
    }
    if(!strcmp(r->method,"PATCH") && !strcmp(r->path,"/api/me")) {
        if(!fields(r,"|nickname|phone|contact_share|history_visible|") || !text_field(in,"nickname",1,80) || !text_field(in,"phone",0,30) ||
            strspn(s(in,"phone"),"0123456789+ ()-")!=strlen(s(in,"phone")) || !bool_field(in,"contact_share") || !bool_field(in,"history_visible"))
            return error(r,422,"INVALID_INPUT","nickname, phone, contact_share and history_visible required");
        execute("UPDATE users SET nickname=%Q,phone=%Q,contact_share=%d,history_visible=%d WHERE id=%lld",s(in,"nickname"),s(in,"phone"),(int)n(in,"contact_share"),(int)n(in,"history_visible"),LL(n(r->user,"id")));
        J *u=first("SELECT * FROM users WHERE id=%lld",LL(n(r->user,"id")));J *o=user_view(u,1);json_object_put(u);return o;
    }
    int64_t id;
    if(get && path_id(r->path,"/api/users/","",&id)) {
        if(!eligible(r->user,r->now) && !is_admin(r))return error(r,403,"NOT_VERIFIED","Verified account required");
        J *u=first("SELECT * FROM users WHERE id=%lld AND role='STUDENT'",LL(id));
        if(!u)return error(r,404,"NOT_FOUND","User not found");
        J *o=user_view(u,id==n(r->user,"id") || is_admin(r));
        if(n(u,"contact_share") && scalar("SELECT count(*) FROM memberships a JOIN memberships b ON a.ride_id=b.ride_id "
            "JOIN rides r ON r.id=a.ride_id WHERE a.user_id=%lld AND b.user_id=%lld AND a.active=1 AND b.active=1 "
            "AND r.status IN ('CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS','COMPLETED')",LL(id),LL(n(r->user,"id")))) js(o,"phone",s(u,"phone"));
        json_object_put(u);return o;
    }
    if(get && !strcmp(r->path,"/api/admin/users")) {
        if(!is_admin(r))return error(r,403,"FORBIDDEN","Administrator required");
        return query("SELECT id,student_id,real_name,nickname,verify_status,suspended,restriction_until FROM users ORDER BY id LIMIT 500");
    }
    if(post && path_id(r->path,"/api/admin/users/","/verify",&id)) {
        if(!is_admin(r))return error(r,403,"FORBIDDEN","Administrator required");
        if(!fields(r,"|decision|reason|") || (strcmp(s(in,"decision"),"APPROVED") && strcmp(s(in,"decision"),"REJECTED")) || !text_field(in,"reason",1,500))
            return error(r,422,"INVALID_INPUT","decision and reason required");
        if(!scalar("SELECT count(*) FROM users WHERE id=%lld AND role='STUDENT'",LL(id)))return error(r,404,"NOT_FOUND","Student not found");
        execute("UPDATE users SET verify_status=%Q WHERE id=%lld",s(in,"decision"),LL(id));
        audit(r,"verify_user",id,s(in,"reason"));
        char key[100];snprintf(key,sizeof key,"verify:%lld:%lld",LL(id),LL(sqlite3_last_insert_rowid(db)));
        notify(id,0,key,s(in,"decision"),r->now);return ok();
    }
    return NULL;
}
