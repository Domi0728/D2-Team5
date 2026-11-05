#include "app.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

J *obj(void) { return json_object_new_object(); }
J *arr(void) { return json_object_new_array(); }
const char *s(J *j, const char *key) {
    J *v = NULL;
    if (!j || !json_object_object_get_ex(j, key, &v) || !json_object_is_type(v, json_type_string)) return "";
    return json_object_get_string(v);
}
int64_t n(J *j, const char *key) {
    J *v = NULL;
    if (!j || !json_object_object_get_ex(j, key, &v)) return 0;
    return json_object_get_int64(v);
}
void js(J *j, const char *key, const char *value) { json_object_object_add(j, key, json_object_new_string(value)); }
void jn(J *j, const char *key, int64_t value) { json_object_object_add(j, key, json_object_new_int64(value)); }
J *ok(void) { J *j = obj(); json_object_object_add(j, "ok", json_object_new_boolean(1)); return j; }
J *error(Request *r, int status, const char *code, const char *message) {
    r->status = status;
    J *j = obj(), *e = obj();
    js(e,"code",code); js(e,"message",message);
    json_object_object_add(j,"error",e);
    return j;
}
int fields(Request *r, const char *allowed) {
    json_object_object_foreach(r->input, key, value) {
        (void)value;
        char token[128];
        if (strlen(key) > 100) return 0;
        snprintf(token,sizeof token,"|%s|",key);
        if (!strstr(allowed,token)) return 0;
    }
    return 1;
}
int text_field(J *j, const char *key, int min, int max) {
    J *v = NULL;
    if (!json_object_object_get_ex(j,key,&v) || !json_object_is_type(v,json_type_string)) return 0;
    const char *value = json_object_get_string(v);
    int len = json_object_get_string_len(v);
    if (len != (int)strlen(value) || len < min || len > max) return 0;
    int nonspace = min == 0;
    for (int i=0;i<len;i++) { if ((unsigned char)value[i]<32) return 0; if (!isspace((unsigned char)value[i])) nonspace=1; }
    return nonspace;
}
int int_field(J *j, const char *key, int64_t min, int64_t max) {
    J *v = NULL;
    return json_object_object_get_ex(j,key,&v) && json_object_is_type(v,json_type_int) && n(j,key)>=min && n(j,key)<=max;
}
int bool_field(J *j,const char *key) {
    J *v=NULL;
    return json_object_object_get_ex(j,key,&v) && json_object_is_type(v,json_type_boolean);
}
static void hex(const unsigned char *in,int len,char *out) {
    static const char *digits="0123456789abcdef";
    for(int i=0;i<len;i++) {out[2*i]=digits[in[i]>>4];out[2*i+1]=digits[in[i]&15];}
    out[len*2]=0;
}
int random_hex(char *out,int bytes) {
    unsigned char b[64];
    if(bytes>64 || RAND_bytes(b,bytes)!=1) return 0;
    hex(b,bytes,out); OPENSSL_cleanse(b,sizeof b); return 1;
}
void digest(const char *input,char out[65]) {
    unsigned char bytes[32]; unsigned int len=0;
    if(EVP_Digest(input,strlen(input),bytes,&len,EVP_sha256(),NULL)!=1) abort();
    hex(bytes,32,out);
}
int strong_password(const char *p) {
    int letter=0,number=0;
    size_t len=strlen(p);
    if(len<10 || len>128) return 0;
    for(size_t i=0;i<len;i++) {letter |= isalpha((unsigned char)p[i]);number |= isdigit((unsigned char)p[i]);}
    return letter && number;
}
static int derive(const char *pass,const char *salt,char out[65]) {
    unsigned char key[32];
    int rc=PKCS5_PBKDF2_HMAC(pass,(int)strlen(pass),(const unsigned char *)salt,32,600000,EVP_sha256(),32,key);
    if(rc==1) hex(key,32,out);
    OPENSSL_cleanse(key,sizeof key); return rc==1;
}
int password_hash(const char *password,char out[162]) {
    char salt[33],hash[65];
    if(!random_hex(salt,16) || !derive(password,salt,hash)) return 0;
    snprintf(out,162,"pbkdf2-sha256$600000$%s$%s",salt,hash); return 1;
}
int password_verify(const char *password,const char *stored) {
    const char *prefix="pbkdf2-sha256$600000$";
    size_t off=strlen(prefix);
    if(strncmp(stored,prefix,off) || strlen(stored)!=off+32+1+64 || stored[off+32]!='$') return 0;
    char salt[33],hash[65];memcpy(salt,stored+off,32);salt[32]=0;
    return derive(password,salt,hash) && CRYPTO_memcmp(hash,stored+off+33,64)==0;
}
const char *arg(Request *r,const char *name) {
    const char *v=MHD_lookup_connection_value(r->connection,MHD_GET_ARGUMENT_KIND,name);
    return v?v:"";
}
int64_t arg_number(Request *r,const char *name,int64_t fallback) {
    const char *v=arg(r,name);if(!*v)return fallback;
    errno=0;char *end;long long x=strtoll(v,&end,10);
    if(errno || *end) {r->status=422;return fallback;}
    return x;
}
int path_id(const char *path,const char *prefix,const char *suffix,int64_t *id) {
    size_t len=strlen(prefix);if(strncmp(path,prefix,len))return 0;
    const char *p=path+len;if(!isdigit((unsigned char)*p))return 0;
    errno=0;char *end;long long x=strtoll(p,&end,10);
    if(errno || x<=0 || strcmp(end,suffix))return 0;
    *id=x;return 1;
}
