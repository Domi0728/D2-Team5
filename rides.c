#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int prestart(J *ride) {
    const char *st=s(ride,"status");
    return !strcmp(st,"WAITING") || !strcmp(st,"CONFIRMED") || !strcmp(st,"RECONFIRMATION_REQUIRED");
}
static int valid_route(int64_t origin,int64_t destination) {
    return origin!=destination && scalar("SELECT count(*) FROM routes r JOIN locations a ON a.id=r.origin_id "
        "JOIN locations b ON b.id=r.destination_id WHERE r.origin_id=%lld AND r.destination_id=%lld "
        "AND r.active=1 AND a.active=1 AND b.active=1",LL(origin),LL(destination))>0;
}
static int valid_ride(Request *r) {
    J *in=r->input;
    return fields(r,"|origin_id|destination_id|departure_at|meeting_point|capacity|tolerance_minutes|luggage_note|expected_version|") &&
        int_field(in,"origin_id",1,INT32_MAX) && int_field(in,"destination_id",1,INT32_MAX) &&
        int_field(in,"departure_at",r->now+1,r->now+366*86400LL) && text_field(in,"meeting_point",1,300) &&
        int_field(in,"capacity",2,cfg("max_capacity")) && int_field(in,"tolerance_minutes",0,cfg("max_tolerance_minutes")) &&
        text_field(in,"luggage_note",0,1000) && valid_route(n(in,"origin_id"),n(in,"destination_id"));
}
static void event(Request *r,int64_t id,const char *type,const char *message) {
    char key[160];
    snprintf(key,sizeof key,"ride:%lld:%s:%lld",LL(id),type,LL(scalar("SELECT version FROM rides WHERE id=%lld",LL(id))));
    notify_members(id,0,key,message,r->now);
    audit(r,type,id,message);
}
static void bump(Request *r,int64_t id) {
    execute("UPDATE rides SET version=version+1,updated_at=%lld WHERE id=%lld",LL(r->now),LL(id));
}
static J *list_rides(Request *r,int search,int mine,int history) {
    int64_t limit=arg_number(r,"limit",50),offset=arg_number(r,"offset",0);
    if(r->status==422 || limit<1 || limit>100 || offset<0 || offset>100000)return error(r,422,"INVALID_QUERY","Invalid pagination");
    J *ids=NULL;
    if(mine || history) {
        ids=query("SELECT DISTINCT r.id FROM rides r JOIN memberships m ON m.ride_id=r.id WHERE m.user_id=%lld "
            "AND %s ORDER BY r.departure_at DESC LIMIT %lld OFFSET %lld",LL(n(r->user,"id")),
            history?"r.status IN ('COMPLETED','CANCELLED','EXPIRED')":"m.active=1 AND r.status NOT IN ('COMPLETED','CANCELLED','EXPIRED')",LL(limit),LL(offset));
    } else if(search) {
        int64_t origin=arg_number(r,"origin_id",0),dest=arg_number(r,"destination_id",0),departure=arg_number(r,"departure_at",0);
        int64_t tolerance=arg_number(r,"tolerance_minutes",cfg("default_tolerance_minutes"));
        int64_t seats=arg_number(r,"min_seats",1);
        const char *sort=arg(r,"sort");
        if(r->status==422 || !valid_route(origin,dest) || departure<=r->now || departure>r->now+366*86400LL || tolerance<0 || tolerance>cfg("max_tolerance_minutes") ||
            seats<1 || seats>6 || (*sort && strcmp(sort,"default") && strcmp(sort,"departure")))
            return error(r,422,"INVALID_QUERY","Provide valid endpoints, future departure_at, tolerance, min_seats and sort");
        ids=query("SELECT r.id,abs(r.departure_at-%lld) AS difference,"
            "(SELECT count(*) FROM rides h JOIN memberships a ON a.ride_id=h.id JOIN memberships b ON b.ride_id=h.id "
            "WHERE h.status='COMPLETED' AND h.closed_at>=%lld AND a.user_id=%lld AND b.user_id=r.organiser_id AND a.active=1 AND b.active=1) AS familiarity "
            "FROM rides r WHERE r.origin_id=%lld AND r.destination_id=%lld AND r.organiser_id<>%lld "
            "AND r.status IN ('WAITING','CONFIRMED') AND r.departure_at>%lld "
            "AND (r.departure_at+28800)/86400=(%lld+28800)/86400 AND abs(r.departure_at-%lld)<=%lld "
            "AND r.capacity-(SELECT count(*) FROM memberships m WHERE m.ride_id=r.id AND m.active=1)>=%lld "
            "AND NOT EXISTS(SELECT 1 FROM memberships m JOIN rides x ON x.id=m.ride_id WHERE m.user_id=%lld AND m.active=1 "
            "AND x.status IN ('WAITING','CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS') AND abs(x.departure_at-r.departure_at)<%d) "
            "ORDER BY %s LIMIT %lld OFFSET %lld",LL(departure),LL(r->now-cfg("retention_days")*86400LL),LL(n(r->user,"id")),
            LL(origin),LL(dest),LL(n(r->user,"id")),LL(r->now),LL(departure),LL(departure),LL(tolerance*60),LL(seats),LL(n(r->user,"id")),
            cfg("overlap_minutes")*60,!strcmp(sort,"departure")?"r.departure_at,r.id":"difference,familiarity DESC,r.created_at,r.id",LL(limit),LL(offset));
    } else {
        ids=query("SELECT id FROM rides WHERE status IN ('WAITING','CONFIRMED') AND departure_at>%lld ORDER BY departure_at,id LIMIT %lld OFFSET %lld",LL(r->now),LL(limit),LL(offset));
    }
    J *out=arr();
    for(size_t i=0;i<json_object_array_length(ids);i++) {
        J *row=json_object_array_get_idx(ids,i),*v=ride_view(n(row,"id"),r);
        if(search) {jn(v,"time_difference_seconds",n(row,"difference"));jn(v,"shared_completed_rides",n(row,"familiarity"));}
        json_object_array_add(out,v);
    }
    json_object_put(ids);return out;
}

static J *invitation_decision(Request *r,int64_t id) {
    J *in=r->input,*inv=first("SELECT * FROM invitations WHERE id=%lld",LL(id));
    if(!inv)return error(r,404,"NOT_FOUND","Invitation not found");
    J *out=NULL,*ride=first("SELECT * FROM rides WHERE id=%lld",LL(n(inv,"ride_id")));
    int64_t uid=n(r->user,"id"),ride_id=n(inv,"ride_id");
    const char *action=s(in,"action");
    if(!fields(r,"|action|expected_version|") || (strcmp(action,"accept") && strcmp(action,"reject") && strcmp(action,"withdraw"))) {
        out=error(r,422,"INVALID_INPUT","action must be accept, reject or withdraw");goto done;
    }
    if((!strcmp(action,"withdraw") && uid!=n(inv,"sender_id")) || (strcmp(action,"withdraw") && uid!=n(inv,"recipient_id"))) {
        out=error(r,403,"FORBIDDEN","Only the sender may withdraw; only the recipient may reply");goto done;
    }
    if(strcmp(s(inv,"status"),"PENDING")) {out=error(r,409,"INVITATION_CLOSED","Invitation already resolved or expired");goto done;}
    if(!ride || !version_matches(r,ride)) {out=error(r,409,"STALE_VERSION","Refresh ride and retry with its current version");goto done;}
    const char *new_status=!strcmp(action,"accept")?"ACCEPTED":!strcmp(action,"reject")?"REJECTED":"WITHDRAWN";
    if(!strcmp(action,"accept")) {
        int is_request=!strcmp(s(inv,"kind"),"REQUEST");
        int64_t joining=is_request?n(inv,"sender_id"):uid;
        J *student=first("SELECT * FROM users WHERE id=%lld",LL(joining));
        int eligible_student=eligible(student,r->now);json_object_put(student);
        if(!eligible(r->user,r->now) || !eligible_student) {out=error(r,403,"INELIGIBLE","Both parties must be verified and unrestricted");goto done;}
        if((strcmp(s(ride,"status"),"WAITING") && strcmp(s(ride,"status"),"CONFIRMED")) || n(ride,"departure_at")<=r->now ||
            n(inv,"deadline")<=r->now || n(inv,"ride_version")!=n(ride,"consent_version")) {
            out=error(r,409,"INVITATION_INVALID","Ride changed, closed or departed");goto done;
        }
        if(!valid_route(n(ride,"origin_id"),n(ride,"destination_id"))) {out=error(r,409,"ROUTE_DISABLED","Route is no longer active");goto done;}
        if(active_member(ride_id,joining)){out=error(r,409,"ALREADY_JOINED","Student already belongs to this ride");goto done;}
        if(member_count(ride_id)>=n(ride,"capacity")){out=error(r,409,"GROUP_FULL","No seats available");goto done;}
        if(overlaps(joining,n(ride,"departure_at"),ride_id)){out=error(r,409,"TIME_CONFLICT","Student already has an overlapping ride");goto done;}
        execute("INSERT INTO memberships(ride_id,user_id,member_type,consent_version,joined_at) VALUES(%lld,%lld,'MEMBER',%lld,%lld) "
            "ON CONFLICT(ride_id,user_id) DO UPDATE SET active=1,consent_version=excluded.consent_version,arrival_flag=0,joined_at=excluded.joined_at",
            LL(ride_id),LL(joining),LL(n(ride,"consent_version")),LL(r->now));
        update_group(ride_id,r->now);bump(r,ride_id);
        execute("UPDATE invitations SET status='WITHDRAWN' WHERE ride_id=%lld AND id<>%lld AND status='PENDING' "
            "AND ((kind='INVITE' AND recipient_id=%lld) OR (kind='REQUEST' AND sender_id=%lld))",LL(ride_id),LL(id),LL(joining),LL(joining));
    }
    execute("UPDATE invitations SET status=%Q WHERE id=%lld",new_status,LL(id));
    char key[100];snprintf(key,sizeof key,"invite:%lld:%s",LL(id),new_status);
    notify(n(inv,"sender_id"),ride_id,key,new_status,r->now);notify(n(inv,"recipient_id"),ride_id,key,new_status,r->now);
    audit(r,"invitation_decision",id,new_status);
    out=obj();jn(out,"id",id);js(out,"status",new_status);json_object_object_add(out,"ride",ride_view(ride_id,r));
done:
    json_object_put(inv);json_object_put(ride);return out;
}

J *ride_routes(Request *r) {
    int get=!strcmp(r->method,"GET"),post=!strcmp(r->method,"POST"),patch=!strcmp(r->method,"PATCH");
    J *in=r->input;
    int64_t uid=n(r->user,"id"),id=0;
    if(get && !strcmp(r->path,"/api/me/history"))return list_rides(r,0,0,1);
    if(get && !strcmp(r->path,"/api/me/rides"))return list_rides(r,0,1,0);
    if(get && !strcmp(r->path,"/api/invitations"))return query("SELECT * FROM invitations WHERE sender_id=%lld OR recipient_id=%lld ORDER BY id DESC LIMIT 200",LL(uid),LL(uid));
    if(post && path_id(r->path,"/api/invitations/","/respond",&id))return invitation_decision(r,id);
    if(strncmp(r->path,"/api/rides",10) && strcmp(r->path,"/api/matches"))return NULL;
    if(!eligible(r->user,r->now) && !is_admin(r))return error(r,403,"INELIGIBLE","Verified and unrestricted student required");
    if(get && !strcmp(r->path,"/api/matches"))return list_rides(r,1,0,0);
    if(get && !strcmp(r->path,"/api/rides"))return list_rides(r,0,0,0);
    if(post && !strcmp(r->path,"/api/rides")) {
        if(!eligible(r->user,r->now))return error(r,403,"STUDENT_REQUIRED","Only verified students may publish");
        if(!valid_ride(r))return error(r,422,"INVALID_RIDE","Check endpoints, time, capacity, meeting point and tolerance");
        if(overlaps(uid,n(in,"departure_at"),0))return error(r,409,"TIME_CONFLICT","You already have an overlapping ride");
        execute("INSERT INTO rides(organiser_id,origin_id,destination_id,origin_name,destination_name,departure_at,meeting_point,capacity,tolerance_minutes,luggage_note,created_at,updated_at) "
            "VALUES(%lld,%lld,%lld,(SELECT name FROM locations WHERE id=%lld),(SELECT name FROM locations WHERE id=%lld),%lld,%Q,%lld,%lld,%Q,%lld,%lld)",
            LL(uid),LL(n(in,"origin_id")),LL(n(in,"destination_id")),LL(n(in,"origin_id")),LL(n(in,"destination_id")),LL(n(in,"departure_at")),s(in,"meeting_point"),
            LL(n(in,"capacity")),LL(n(in,"tolerance_minutes")),s(in,"luggage_note"),LL(r->now),LL(r->now));
        id=sqlite3_last_insert_rowid(db);
        execute("INSERT INTO memberships(ride_id,user_id,member_type,consent_version,joined_at) VALUES(%lld,%lld,'ORGANISER',1,%lld)",LL(id),LL(uid),LL(r->now));
        audit(r,"publish_ride",id,"Created");return ride_view(id,r);
    }
    const char *action=NULL;
    const char *actions[]={"","/invitations","/join-requests","/withdraw","/cancel","/reconfirm","/start","/arrive","/reuse"};
    for(size_t i=0;i<sizeof actions/sizeof actions[0];i++)if(path_id(r->path,"/api/rides/",actions[i],&id)){action=actions[i];break;}
    if(!action)return NULL;
    J *ride=first("SELECT * FROM rides WHERE id=%lld",LL(id)),*out=NULL;
    if(!ride)return error(r,404,"NOT_FOUND","Ride not found");
    if(get && !*action) {
        if(!prestart(ride) && !is_admin(r) && !scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND user_id=%lld",LL(id),LL(uid)))
            out=error(r,403,"PRIVATE_HISTORY","Only participants may view closed or ongoing ride details");
        else out=ride_view(id,r);
        goto done;
    }
    if(!post && !patch){out=error(r,405,"METHOD_NOT_ALLOWED","Use the documented HTTP method");goto done;}
    if(is_admin(r)){out=error(r,403,"STUDENT_REQUIRED","This operation requires the student actor");goto done;}
    if(post && !strcmp(action,"/reuse")) {
        if(!scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND user_id=%lld",LL(id),LL(uid))) {out=error(r,403,"FORBIDDEN","Only your own past rides can be reused");goto done;}
        if(prestart(ride) || !strcmp(s(ride,"status"),"IN_PROGRESS")){out=error(r,409,"NOT_HISTORY","Ride is not closed yet");goto done;}
        if(!valid_route(n(ride,"origin_id"),n(ride,"destination_id"))){out=error(r,409,"ROUTE_DISABLED","This route is no longer available");goto done;}
        out=obj();jn(out,"origin_id",n(ride,"origin_id"));jn(out,"destination_id",n(ride,"destination_id"));
        js(out,"meeting_point",s(ride,"meeting_point"));js(out,"luggage_note",s(ride,"luggage_note"));
        js(out,"next_step","Choose a new departure_at and capacity, then POST /api/rides");goto done;
    }
    if(!version_matches(r,ride)){out=error(r,409,"STALE_VERSION","Refresh the ride and use its current expected_version");goto done;}
    if(patch && !*action) {
        if(uid!=n(ride,"organiser_id")){out=error(r,403,"FORBIDDEN","Only the organiser may edit");goto done;}
        if(!prestart(ride) || n(ride,"departure_at")<=r->now){out=error(r,409,"RIDE_CLOSED","Editing is only allowed before departure");goto done;}
        if(!valid_ride(r) || n(in,"capacity")<member_count(id)){out=error(r,422,"INVALID_RIDE","Invalid changes or capacity below member count");goto done;}
        int material=n(in,"origin_id")!=n(ride,"origin_id") || n(in,"destination_id")!=n(ride,"destination_id") ||
            n(in,"departure_at")!=n(ride,"departure_at") || strcmp(s(in,"meeting_point"),s(ride,"meeting_point"));
        J *members=query("SELECT user_id FROM memberships WHERE ride_id=%lld AND active=1",LL(id));
        for(size_t i=0;i<json_object_array_length(members);i++)if(overlaps(n(json_object_array_get_idx(members,i),"user_id"),n(in,"departure_at"),id)) {
            json_object_put(members);out=error(r,409,"TIME_CONFLICT","New departure overlaps another member ride");goto done;
        }
        json_object_put(members);
        execute("INSERT INTO ride_changes(ride_id,version,actor_id,snapshot,created_at) VALUES(%lld,%lld,%lld,%Q,%lld)",LL(id),LL(n(ride,"version")),LL(uid),json_object_to_json_string_ext(ride,JSON_C_TO_STRING_PLAIN),LL(r->now));
        execute("UPDATE rides SET origin_id=%lld,destination_id=%lld,origin_name=(SELECT name FROM locations WHERE id=%lld),"
            "destination_name=(SELECT name FROM locations WHERE id=%lld),departure_at=%lld,meeting_point=%Q,capacity=%lld,tolerance_minutes=%lld,luggage_note=%Q,consent_version=consent_version+%d WHERE id=%lld",
            LL(n(in,"origin_id")),LL(n(in,"destination_id")),LL(n(in,"origin_id")),LL(n(in,"destination_id")),LL(n(in,"departure_at")),s(in,"meeting_point"),LL(n(in,"capacity")),
            LL(n(in,"tolerance_minutes")),s(in,"luggage_note"),material?1:0,LL(id));
        if(material) {
            execute("UPDATE memberships SET consent_version=(SELECT consent_version FROM rides WHERE id=%lld) WHERE ride_id=%lld AND user_id=%lld",LL(id),LL(id),LL(uid));
            execute("UPDATE invitations SET status='WITHDRAWN' WHERE ride_id=%lld AND status='PENDING'",LL(id));
            update_group(id,r->now);
        }
        bump(r,id);event(r,id,"edit",material?"Ride changed; reconfirm the latest plan":"Ride details updated");
        out=ride_view(id,r);goto done;
    }
    if(post && (!strcmp(action,"/invitations") || !strcmp(action,"/join-requests"))) {
        if(!fields(r,"|recipient_id|expected_version|")){out=error(r,422,"INVALID_INPUT","Unexpected fields");goto done;}
        int is_request=!strcmp(action,"/join-requests");
        int64_t target=is_request?n(ride,"organiser_id"):n(in,"recipient_id");
        if((!is_request && uid!=n(ride,"organiser_id")) || (is_request && uid==n(ride,"organiser_id"))) {out=error(r,403,"FORBIDDEN","Invalid invitation sender");goto done;}
        if((strcmp(s(ride,"status"),"WAITING") && strcmp(s(ride,"status"),"CONFIRMED")) || n(ride,"departure_at")<=r->now) {out=error(r,409,"RIDE_CLOSED","Ride is not accepting members");goto done;}
        J *u=first("SELECT * FROM users WHERE id=%lld",LL(target));int valid=eligible(u,r->now);json_object_put(u);
        int64_t joining=is_request?uid:target;
        if(!valid || active_member(id,joining)){out=error(r,409,"INELIGIBLE","Recipient unavailable or student already joined");goto done;}
        if(member_count(id)>=n(ride,"capacity")){out=error(r,409,"GROUP_FULL","No seats available");goto done;}
        if(overlaps(joining,n(ride,"departure_at"),id)){out=error(r,409,"TIME_CONFLICT","Student has an overlapping ride");goto done;}
        if(scalar("SELECT count(*) FROM invitations WHERE ride_id=%lld AND status='PENDING' AND "
            "((kind='INVITE' AND recipient_id=%lld) OR (kind='REQUEST' AND sender_id=%lld))",LL(id),LL(joining),LL(joining))) {out=error(r,409,"DUPLICATE_INVITATION","A pending invitation or join request already exists");goto done;}
        int64_t deadline=r->now+cfg("invite_minutes")*60;if(deadline>n(ride,"departure_at"))deadline=n(ride,"departure_at");
        execute("INSERT INTO invitations(ride_id,sender_id,recipient_id,kind,ride_version,created_at,deadline) VALUES(%lld,%lld,%lld,%Q,%lld,%lld,%lld)",
            LL(id),LL(uid),LL(target),is_request?"REQUEST":"INVITE",LL(n(ride,"consent_version")),LL(r->now),LL(deadline));
        int64_t inv_id=sqlite3_last_insert_rowid(db);char key[100];snprintf(key,sizeof key,"invite:%lld:new",LL(inv_id));
        notify(target,id,key,is_request?"New join request":"New ride invitation",r->now);
        out=first("SELECT * FROM invitations WHERE id=%lld",LL(inv_id));goto done;
    }
    if(!post){out=error(r,405,"METHOD_NOT_ALLOWED","Use POST for ride actions");goto done;}
    if(!fields(r,"|expected_version|reason|")){out=error(r,422,"INVALID_INPUT","Unexpected fields");goto done;}
    if(!active_member(id,uid)){out=error(r,403,"FORBIDDEN","Active ride member required");goto done;}
    if(!strcmp(action,"/withdraw") || !strcmp(action,"/cancel")) {
        int cancel=!strcmp(action,"/cancel");
        if(!prestart(ride) || n(ride,"departure_at")<=r->now){out=error(r,409,"RIDE_CLOSED","Cancellation and withdrawal require a future pre-start ride");goto done;}
        if((cancel && uid!=n(ride,"organiser_id")) || (!cancel && uid==n(ride,"organiser_id"))) {out=error(r,403,"FORBIDDEN","Organiser cancels the ride; other members withdraw");goto done;}
        J *reason=NULL;
        if(json_object_object_get_ex(in,"reason",&reason) && !text_field(in,"reason",0,500)){out=error(r,422,"INVALID_INPUT","Invalid reason");goto done;}
        if(cancel) {
            execute("UPDATE rides SET status='CANCELLED',cancel_reason=%Q,closed_at=%lld WHERE id=%lld",s(in,"reason"),LL(r->now),LL(id));
            execute("UPDATE invitations SET status='WITHDRAWN' WHERE ride_id=%lld AND status='PENDING'",LL(id));
        } else {
            execute("UPDATE memberships SET active=0 WHERE ride_id=%lld AND user_id=%lld",LL(id),LL(uid));update_group(id,r->now);
        }
        bump(r,id);event(r,id,cancel?"cancel":"withdraw",cancel?"Ride cancelled":"A member withdrew");
        out=ride_view(id,r);jn(out,"late_cancellation_warning",n(ride,"departure_at")-r->now<=cfg("late_cancel_minutes")*60);goto done;
    }
    if(!strcmp(action,"/reconfirm")) {
        if(strcmp(s(ride,"status"),"RECONFIRMATION_REQUIRED") || n(ride,"departure_at")<=r->now){out=error(r,409,"INVALID_STATE","Ride does not await reconfirmation");goto done;}
        execute("UPDATE memberships SET consent_version=%lld WHERE ride_id=%lld AND user_id=%lld",LL(n(ride,"consent_version")),LL(id),LL(uid));
        update_group(id,r->now);bump(r,id);event(r,id,"reconfirm","A member confirmed the revised plan");out=ride_view(id,r);goto done;
    }
    if(!strcmp(action,"/start")) {
        if(uid!=n(ride,"organiser_id")){out=error(r,403,"FORBIDDEN","Only the organiser can start");goto done;}
        if(strcmp(s(ride,"status"),"CONFIRMED") || r->now<n(ride,"departure_at") || member_count(id)<2 ||
            scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND active=1 AND consent_version<>%lld",LL(id),LL(n(ride,"consent_version")))) {
            out=error(r,409,"INVALID_STATE","Require departure time, at least two members and current consent");goto done;
        }
        execute("UPDATE rides SET status='IN_PROGRESS' WHERE id=%lld",LL(id));
        execute("UPDATE invitations SET status='WITHDRAWN' WHERE ride_id=%lld AND status='PENDING'",LL(id));
        bump(r,id);event(r,id,"start","Ride started");out=ride_view(id,r);goto done;
    }
    if(!strcmp(action,"/arrive")) {
        if(strcmp(s(ride,"status"),"IN_PROGRESS")){out=error(r,409,"INVALID_STATE","Ride is not in progress");goto done;}
        execute("UPDATE memberships SET arrival_flag=1 WHERE ride_id=%lld AND user_id=%lld",LL(id),LL(uid));
        if(!scalar("SELECT count(*) FROM memberships WHERE ride_id=%lld AND active=1 AND arrival_flag=0",LL(id)))
            execute("UPDATE rides SET status='COMPLETED',closed_at=%lld WHERE id=%lld",LL(r->now),LL(id));
        bump(r,id);event(r,id,"arrive","Arrival confirmation updated");out=ride_view(id,r);goto done;
    }
    out=error(r,404,"NOT_FOUND","Unknown action");
done:
    json_object_put(ride);return out;
}
