#include "app.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/crypto.h>

const char *allowed_origins;
static volatile sig_atomic_t running=1;
typedef struct {char *body;size_t size;int answered;} Upload;

static void stop_server(int signal_number) {(void)signal_number;running=0;}
static int origin_allowed(const char *origin) {
    if(!origin || !*origin)return 0;
    size_t n=strlen(origin);const char *p=allowed_origins;
    while(*p) {
        const char *end=strchr(p,',');size_t len=end?(size_t)(end-p):strlen(p);
        if(len==n && !strncmp(p,origin,n))return 1;
        if(!end)break;
        p=end+1;
    }
    return 0;
}
static enum MHD_Result respond(struct MHD_Connection *c,int status,J *value) {
    const char *text=json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
    struct MHD_Response *response=MHD_create_response_from_buffer(strlen(text),(void *)text,MHD_RESPMEM_MUST_COPY);
    json_object_put(value);
    if(!response)return MHD_NO;
    MHD_add_response_header(response,"Content-Type","application/json; charset=utf-8");
    MHD_add_response_header(response,"Cache-Control","no-store");
    MHD_add_response_header(response,"X-Content-Type-Options","nosniff");
    const char *origin=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Origin");
    if(origin_allowed(origin)) {
        MHD_add_response_header(response,"Access-Control-Allow-Origin",origin);
        MHD_add_response_header(response,"Vary","Origin");
        MHD_add_response_header(response,"Access-Control-Allow-Methods","GET, POST, PATCH, OPTIONS");
        MHD_add_response_header(response,"Access-Control-Allow-Headers","Authorization, Content-Type, Idempotency-Key");
    }
    enum MHD_Result ret=MHD_queue_response(c,(unsigned int)status,response);
    MHD_destroy_response(response);return ret;
}
static enum MHD_Result access_request(void *cls,struct MHD_Connection *c,const char *url,const char *method,
    const char *version,const char *upload_data,size_t *upload_size,void **con_cls) {
    (void)cls;(void)version;
    if(!*con_cls) {
        Upload *u=calloc(1,sizeof *u);if(!u)return MHD_NO;
        u->body=calloc(65537,1);if(!u->body){free(u);return MHD_NO;}
        *con_cls=u;return MHD_YES;
    }
    Upload *u=*con_cls;if(u->answered)return MHD_YES;
    Request r={.method=method,.path=url,.connection=c,.status=200,.body=u->body,.token="",.key=""};
    if(*upload_size) {
        if(*upload_size>65536-u->size) {u->answered=1;return respond(c,413,error(&r,413,"BODY_TOO_LARGE","Maximum JSON body size is 64 KiB"));}
        memcpy(u->body+u->size,upload_data,*upload_size);u->size+=*upload_size;u->body[u->size]=0;*upload_size=0;return MHD_YES;
    }
    u->answered=1;
    if(!strcmp(method,"OPTIONS"))return respond(c,200,ok());
    if(strcmp(method,"GET") && strcmp(method,"POST") && strcmp(method,"PATCH"))return respond(c,405,error(&r,405,"METHOD_NOT_ALLOWED","Use GET, POST or PATCH"));
    if(strlen(url)>300 || u->size!=strlen(u->body))return respond(c,400,error(&r,400,"INVALID_REQUEST","Invalid request"));
    if(u->size) {
        const char *content=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Content-Type");
        if(!content || strncasecmp(content,"application/json",16) || (content[16] && content[16]!=';'))return respond(c,415,error(&r,415,"JSON_REQUIRED","Use Content-Type: application/json"));
        struct json_tokener *tok=json_tokener_new();
        json_tokener_set_flags(tok,JSON_TOKENER_STRICT|JSON_TOKENER_VALIDATE_UTF8);
        r.input=json_tokener_parse_ex(tok,u->body,(int)u->size);
        int valid=json_tokener_get_error(tok)==json_tokener_success && json_object_is_type(r.input,json_type_object);
        json_tokener_free(tok);
        if(!valid){json_object_put(r.input);return respond(c,400,error(&r,400,"INVALID_JSON","A valid JSON object is required"));}
    } else r.input=obj();
    const char *auth=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Authorization");
    if(auth && !strncmp(auth,"Bearer ",7) && strlen(auth)<200)r.token=auth+7;
    const char *key=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Idempotency-Key");if(key)r.key=key;
    J *result=handle_request(&r);json_object_put(r.input);
    return respond(c,r.status,result);
}
static void completed(void *cls,struct MHD_Connection *c,void **con_cls,enum MHD_RequestTerminationCode code) {
    (void)cls;(void)c;(void)code;Upload *u=*con_cls;
    if(u){if(u->body){OPENSSL_cleanse(u->body,u->size);free(u->body);}free(u);*con_cls=NULL;}
}
static int seed_locations(void) {
    execute("BEGIN IMMEDIATE");
    execute("INSERT OR IGNORE INTO locations(id,name) VALUES(1,'MUST Campus'),(2,'Hengqin Port'),(3,'Hengqin Residential Area')");
    execute("INSERT OR IGNORE INTO location_aliases SELECT name,id FROM locations");
    execute("INSERT OR IGNORE INTO routes SELECT a.id,b.id,1 FROM locations a CROSS JOIN locations b WHERE a.id<>b.id");
    execute(db_failed?"ROLLBACK":"COMMIT");return db_failed?1:0;
}
int main(int argc,char **argv) {
    umask(0077);
    const char *path=getenv("RIDE_DB");if(!path)path="data/ride.db";
    mkdir("data",0700);db_init(path);
    if(argc==3 && !strcmp(argv[1],"--create-admin")) {
        if(strlen(argv[2])<1 || strlen(argv[2])>80 || scalar("SELECT count(*) FROM users WHERE student_id=%Q",argv[2])) {fprintf(stderr,"Invalid or existing account name\n");return 1;}
        const char *password=getenv("RIDE_ADMIN_PASSWORD");if(!password)password=getpass("New administrator password: ");
        if(!password || !strong_password(password)){fprintf(stderr,"Use 10+ characters with letters and digits\n");return 1;}
        char hash[162];if(!password_hash(password,hash))return 1;
        execute("INSERT INTO users(student_id,real_name,nickname,password_hash,role,verify_status,created_at) VALUES(%Q,'Administrator','Administrator',%Q,'ADMIN','APPROVED',%lld)",argv[2],hash,LL(now_seconds()));
        sqlite3_close(db);puts("Administrator created");return db_failed?1:0;
    }
    if(argc==2 && !strcmp(argv[1],"--seed")){int rc=seed_locations();sqlite3_close(db);puts("Demo locations initialized");return rc;}
    if(argc==3 && !strcmp(argv[1],"--backup")) {
        struct stat existing;if(stat(argv[2],&existing)==0){fprintf(stderr,"Backup path already exists\n");return 1;}
        sqlite3 *dest=NULL;if(sqlite3_open(argv[2],&dest)!=SQLITE_OK)return 1;
        sqlite3_backup *backup=sqlite3_backup_init(dest,"main",db,"main");
        int rc=backup?sqlite3_backup_step(backup,-1):SQLITE_ERROR;
        if(backup)sqlite3_backup_finish(backup);
        sqlite3_close(dest);sqlite3_close(db);if(rc!=SQLITE_DONE)return 1;puts("Backup created");return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--test-clock")){test_clock_path=argv[2];fprintf(stderr,"Test clock enabled; do not use for deployment\n");}
    else if(argc!=1){fprintf(stderr,"Usage: %s [--create-admin NAME | --seed | --backup FILE | --test-clock FILE]\n",argv[0]);return 1;}
    const char *port_env=getenv("RIDE_PORT"),*bind=getenv("RIDE_BIND");
    char *port_end=NULL;long port=port_env?strtol(port_env,&port_end,10):8080;
    if(port<1 || port>65535 || (port_env && *port_end)){fprintf(stderr,"Invalid RIDE_PORT\n");return 1;}
    if(!bind)bind="127.0.0.1";
    allowed_origins=getenv("RIDE_ORIGINS");if(!allowed_origins)allowed_origins="http://localhost:5173,http://127.0.0.1:5173,http://localhost:3000";
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons((uint16_t)port);
    if(inet_pton(AF_INET,bind,&addr.sin_addr)!=1){fprintf(stderr,"Invalid RIDE_BIND IPv4 address\n");return 1;}
    signal(SIGINT,stop_server);signal(SIGTERM,stop_server);signal(SIGPIPE,SIG_IGN);
    struct MHD_Daemon *daemon=MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,(uint16_t)port,NULL,NULL,access_request,NULL,
        MHD_OPTION_SOCK_ADDR,(struct sockaddr *)&addr,MHD_OPTION_NOTIFY_COMPLETED,completed,NULL,
        MHD_OPTION_CONNECTION_LIMIT,(unsigned int)64,MHD_OPTION_CONNECTION_TIMEOUT,(unsigned int)15,
        MHD_OPTION_THREAD_POOL_SIZE,(unsigned int)8,MHD_OPTION_END);
    if(!daemon){fprintf(stderr,"Cannot start HTTP server\n");sqlite3_close(db);return 1;}
    printf("Campus Ride C backend: http://%s:%ld\n",bind,port);fflush(stdout);
    while(running) {
        sleep(2);pthread_mutex_lock(&db_mutex);db_failed=0;execute("BEGIN IMMEDIATE");maintenance(now_seconds());
        execute(db_failed?"ROLLBACK":"COMMIT");pthread_mutex_unlock(&db_mutex);
    }
    MHD_stop_daemon(daemon);sqlite3_close(db);return 0;
}
