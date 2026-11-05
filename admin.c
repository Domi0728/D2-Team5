#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_aliases(J *in) {
    J *aliases=NULL;
    if(!json_object_object_get_ex(in,"aliases",&aliases) || !json_object_is_type(aliases,json_type_array) || json_object_array_length(aliases)>20)return 0;
    for(size_t i=0;i<json_object_array_length(aliases);i++) {
        J *v=json_object_array_get_idx(aliases,i);
        if(!json_object_is_type(v,json_type_string) || strlen(json_object_get_string(v))<1 || strlen(json_object_get_string(v))>100 ||
            (size_t)json_object_get_string_len(v)!=strlen(json_object_get_string(v)))return 0;
    }
    return 1;
}

J *other_routes(Request *r) {
    int get=!strcmp(r->method,"GET"),post=!strcmp(r->method,"POST"),patch=!strcmp(r->method,"PATCH");
    J *in=r->input;int64_t uid=n(r->user,"id"),id=0;
    if(get && !strcmp(r->path,"/api/locations")) {
        J *rows=query("SELECT l.*,coalesce((SELECT json_group_array(alias) FROM location_aliases a WHERE a.location_id=l.id),'[]') AS aliases FROM locations l WHERE l.active=1 ORDER BY l.id");
        for(size_t i=0;i<json_object_array_length(rows);i++) {
            J *row=json_object_array_get_idx(rows,i);
            J *aliases=json_tokener_parse(s(row,"aliases"));
            json_object_object_add(row,"aliases",aliases?aliases:arr());
        }
        return rows;
    }
    if(get && !strcmp(r->path,"/api/routes"))return query("SELECT r.*,a.name AS origin_name,b.name AS destination_name FROM routes r JOIN locations a ON a.id=r.origin_id JOIN locations b ON b.id=r.destination_id WHERE r.active=1 AND a.active=1 AND b.active=1");
    if(get && !strcmp(r->path,"/api/settings"))return query("SELECT key,value FROM config ORDER BY key");
    if(get && !strcmp(r->path,"/api/notifications")) {
        int64_t limit=arg_number(r,"limit",50),offset=arg_number(r,"offset",0);
        if(r->status==422 || limit<1 || limit>100 || offset<0 || offset>100000)return error(r,422,"INVALID_QUERY","Invalid pagination");
        J *o=obj();jn(o,"unread_count",scalar("SELECT count(*) FROM notifications WHERE user_id=%lld AND is_read=0",LL(uid)));
        json_object_object_add(o,"items",query("SELECT id,ride_id,content,is_read,created_at FROM notifications WHERE user_id=%lld ORDER BY id DESC LIMIT %lld OFFSET %lld",LL(uid),LL(limit),LL(offset)));return o;
    }
    if(post && path_id(r->path,"/api/notifications/","/read",&id)) {
        if(!scalar("SELECT count(*) FROM notifications WHERE id=%lld AND user_id=%lld",LL(id),LL(uid)))return error(r,404,"NOT_FOUND","Notification not found");
        execute("UPDATE notifications SET is_read=1 WHERE id=%lld",LL(id));return ok();
    }
    if(get && !strcmp(r->path,"/api/reports"))return query("SELECT * FROM reports WHERE reporter_id=%lld ORDER BY id DESC LIMIT 200",LL(uid));
    if(post && !strcmp(r->path,"/api/reports")) {
        if(strcmp(s(r->user,"role"),"STUDENT") || strcmp(s(r->user,"verify_status"),"APPROVED"))return error(r,403,"NOT_VERIFIED","Verified student required");
        const char *category=s(in,"category");
        if(!fields(r,"|ride_id|target_user_id|category|description|") || !int_field(in,"ride_id",1,INT32_MAX) || !int_field(in,"target_user_id",1,INT32_MAX) ||
            !text_field(in,"description",5,2000) || (strcmp(category,"NO_SHOW") && strcmp(category,"LATE") && strcmp(category,"BEHAVIOUR") && strcmp(category,"OTHER")))
            return error(r,422,"INVALID_INPUT","Invalid report fields");
        int64_t ride=n(in,"ride_id"),target=n(in,"target_user_id");
        if(uid==target || !scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND user_id=%lld",LL(ride),LL(uid)) ||
            !scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND user_id=%lld",LL(ride),LL(target)))
            return error(r,403,"FORBIDDEN","Report another participant from one of your rides");
        if(scalar("SELECT count(*) FROM reports WHERE reporter_id=%lld AND ride_id=%lld AND target_user_id=%lld AND category=%Q",LL(uid),LL(ride),LL(target),category))
            return error(r,409,"DUPLICATE_REPORT","Report already submitted");
        execute("INSERT INTO reports(reporter_id,ride_id,target_user_id,category,description,created_at) VALUES(%lld,%lld,%lld,%Q,%Q,%lld)",LL(uid),LL(ride),LL(target),category,s(in,"description"),LL(r->now));
        return first("SELECT * FROM reports WHERE id=%lld",LL(sqlite3_last_insert_rowid(db)));
    }
    if(get && path_id(r->path,"/api/users/","/history",&id)) {
        if(!eligible(r->user,r->now) && !is_admin(r))return error(r,403,"NOT_VERIFIED","Verified account required");
        if(!scalar("SELECT count(*) FROM users WHERE id=%lld AND history_visible=1",LL(id)))return error(r,404,"NOT_FOUND","History is not available");
        return query("SELECT r.origin_name,r.destination_name,r.departure_at,r.status FROM rides r JOIN memberships m ON m.ride_id=r.id "
            "WHERE m.user_id=%lld AND m.active=1 AND r.status IN ('COMPLETED','CANCELLED','EXPIRED') ORDER BY r.departure_at DESC LIMIT 100",LL(id));
    }
    if(strncmp(r->path,"/api/admin/",11))return NULL;
    if(!is_admin(r))return error(r,403,"FORBIDDEN","Administrator required");
    if(get && !strcmp(r->path,"/api/admin/audit"))return query("SELECT * FROM audit_logs ORDER BY id DESC LIMIT 200");
    if(get && !strcmp(r->path,"/api/admin/reports"))return query("SELECT * FROM reports ORDER BY id DESC LIMIT 200");
    if(get && !strcmp(r->path,"/api/admin/locations"))return query("SELECT * FROM locations ORDER BY id");
    if(post && !strcmp(r->path,"/api/admin/locations")) {
        if(!fields(r,"|name|aliases|") || !text_field(in,"name",1,100) || !valid_aliases(in))return error(r,422,"INVALID_INPUT","name and aliases array required");
        J *aliases=NULL;json_object_object_get_ex(in,"aliases",&aliases);
        if(scalar("SELECT count(*) FROM locations WHERE name=%Q COLLATE NOCASE",s(in,"name")) || scalar("SELECT count(*) FROM location_aliases WHERE alias=%Q",s(in,"name")))
            return error(r,409,"DUPLICATE_LOCATION","Location name already exists");
        for(size_t i=0;i<json_object_array_length(aliases);i++) {
            const char *a=json_object_get_string(json_object_array_get_idx(aliases,i));
            if(scalar("SELECT count(*) FROM location_aliases WHERE alias=%Q",a) || scalar("SELECT count(*) FROM locations WHERE name=%Q COLLATE NOCASE",a))
                return error(r,409,"ALIAS_CONFLICT","Alias is already assigned");
        }
        execute("INSERT INTO locations(name) VALUES(%Q)",s(in,"name"));id=sqlite3_last_insert_rowid(db);
        execute("INSERT INTO location_aliases VALUES(%Q,%lld)",s(in,"name"),LL(id));
        for(size_t i=0;i<json_object_array_length(aliases);i++)execute("INSERT OR IGNORE INTO location_aliases VALUES(%Q,%lld)",json_object_get_string(json_object_array_get_idx(aliases,i)),LL(id));
        audit(r,"create_location",id,s(in,"name"));return first("SELECT * FROM locations WHERE id=%lld",LL(id));
    }
    if(get && path_id(r->path,"/api/admin/locations/","/impact",&id)) {
        if(!scalar("SELECT count(*) FROM locations WHERE id=%lld",LL(id)))return error(r,404,"NOT_FOUND","Location not found");
        J *o=obj();jn(o,"affected_rides",scalar("SELECT count(*) FROM rides WHERE (origin_id=%lld OR destination_id=%lld) AND status IN ('WAITING','CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS')",LL(id),LL(id)));return o;
    }
    if(patch && path_id(r->path,"/api/admin/locations/","",&id)) {
        if(!fields(r,"|active|confirm_affected|") || !bool_field(in,"active") || !bool_field(in,"confirm_affected"))return error(r,422,"INVALID_INPUT","active and confirm_affected booleans required");
        if(!scalar("SELECT count(*) FROM locations WHERE id=%lld",LL(id)))return error(r,404,"NOT_FOUND","Location not found");
        int64_t count=scalar("SELECT count(*) FROM rides WHERE (origin_id=%lld OR destination_id=%lld) AND status IN ('WAITING','CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS')",LL(id),LL(id));
        if(!n(in,"active") && count && !n(in,"confirm_affected"))return error(r,409,"CONFIRM_IMPACT","Read impact endpoint, then confirm_affected=true to deactivate");
        execute("UPDATE locations SET active=%d WHERE id=%lld",(int)n(in,"active"),LL(id));audit(r,"location_status",id,n(in,"active")?"Activated":"Deactivated; historical rides preserved");return ok();
    }
    if(post && !strcmp(r->path,"/api/admin/routes")) {
        if(!fields(r,"|origin_id|destination_id|active|") || !int_field(in,"origin_id",1,INT32_MAX) || !int_field(in,"destination_id",1,INT32_MAX) || !bool_field(in,"active") || n(in,"origin_id")==n(in,"destination_id"))
            return error(r,422,"INVALID_INPUT","Two different location IDs and active required");
        if(scalar("SELECT count(*) FROM locations WHERE id IN (%lld,%lld)",LL(n(in,"origin_id")),LL(n(in,"destination_id")))!=2)return error(r,404,"NOT_FOUND","Location not found");
        execute("INSERT INTO routes VALUES(%lld,%lld,%d) ON CONFLICT(origin_id,destination_id) DO UPDATE SET active=excluded.active",LL(n(in,"origin_id")),LL(n(in,"destination_id")),(int)n(in,"active"));
        audit(r,"configure_route",n(in,"origin_id"),json_object_to_json_string_ext(in,JSON_C_TO_STRING_PLAIN));return ok();
    }
    if(get && !strcmp(r->path,"/api/admin/config"))return query("SELECT key,value FROM config ORDER BY key");
    if(patch && !strcmp(r->path,"/api/admin/config")) {
        if(!fields(r,"|default_tolerance_minutes|max_tolerance_minutes|invite_minutes|max_capacity|late_cancel_minutes|overlap_minutes|start_grace_minutes|retention_days|") ||
            !int_field(in,"default_tolerance_minutes",0,120) || !int_field(in,"max_tolerance_minutes",0,120) || n(in,"default_tolerance_minutes")>n(in,"max_tolerance_minutes") ||
            !int_field(in,"invite_minutes",1,30) || !int_field(in,"max_capacity",2,6) || !int_field(in,"late_cancel_minutes",0,60) ||
            !int_field(in,"overlap_minutes",1,240) || !int_field(in,"start_grace_minutes",1,120) || !int_field(in,"retention_days",1,365))return error(r,422,"INVALID_INPUT","Provide all settings within documented bounds");
        json_object_object_foreach(in,key,value) {execute("UPDATE config SET value=%lld WHERE key=%Q",LL(json_object_get_int64(value)),key);}
        audit(r,"configure_system",0,json_object_to_json_string_ext(in,JSON_C_TO_STRING_PLAIN));return ok();
    }
    if(post && path_id(r->path,"/api/admin/reports/","/review",&id)) {
        const char *action=s(in,"action");
        if(!fields(r,"|action|note|restriction_hours|") || !text_field(in,"note",1,1000) ||
            (strcmp(action,"DISMISS") && strcmp(action,"WARN") && strcmp(action,"RESTRICT") && strcmp(action,"SUSPEND")) ||
            (!strcmp(action,"RESTRICT") && !int_field(in,"restriction_hours",1,720)))return error(r,422,"INVALID_INPUT","Invalid moderation decision");
        J *report=first("SELECT * FROM reports WHERE id=%lld",LL(id));
        if(!report)return error(r,404,"NOT_FOUND","Report not found");
        if(strcmp(s(report,"status"),"PENDING")){json_object_put(report);return error(r,409,"ALREADY_REVIEWED","Report already reviewed");}
        int64_t target=n(report,"target_user_id");
        if(!strcmp(action,"RESTRICT"))execute("UPDATE users SET restriction_until=max(coalesce(restriction_until,0),%lld) WHERE id=%lld",LL(r->now+n(in,"restriction_hours")*3600),LL(target));
        if(!strcmp(action,"SUSPEND"))execute("UPDATE users SET suspended=1 WHERE id=%lld",LL(target));
        execute("UPDATE reports SET status='REVIEWED',decision=%Q,moderation_note=%Q,reviewed_at=%lld WHERE id=%lld",action,s(in,"note"),LL(r->now),LL(id));
        char key[100];snprintf(key,sizeof key,"report:%lld:reviewed",LL(id));
        notify(n(report,"reporter_id"),n(report,"ride_id"),key,"Your report has been reviewed",r->now);
        if(strcmp(action,"DISMISS"))notify(target,n(report,"ride_id"),key,action,r->now);
        audit(r,"review_report",id,s(in,"note"));json_object_put(report);return ok();
    }
    if(post && path_id(r->path,"/api/admin/users/","/restore",&id)) {
        if(!text_field(in,"reason",1,500))return error(r,422,"INVALID_INPUT","Reason required");
        if(!scalar("SELECT count(*) FROM users WHERE id=%lld AND role='STUDENT'",LL(id)))return error(r,404,"NOT_FOUND","Student not found");
        execute("UPDATE users SET suspended=0,restriction_until=NULL WHERE id=%lld",LL(id));audit(r,"restore_user",id,s(in,"reason"));return ok();
    }
    if(post && !strcmp(r->path,"/api/admin/maintenance")) {
        int64_t cutoff=r->now-cfg("retention_days")*86400LL;
        execute("DELETE FROM notifications WHERE created_at<%lld",LL(cutoff));
        execute("DELETE FROM reports WHERE status='REVIEWED' AND reviewed_at<%lld",LL(cutoff));
        execute("DELETE FROM rides WHERE closed_at<%lld AND NOT EXISTS(SELECT 1 FROM reports p WHERE p.ride_id=rides.id)",LL(cutoff));
        int count=sqlite3_changes(db);audit(r,"retention_cleanup",0,"Removed eligible expired history; open reports retained");
        J *o=ok();jn(o,"deleted_rides",count);return o;
    }
    return NULL;
}
