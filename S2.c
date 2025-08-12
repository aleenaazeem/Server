// S2.c — PDF backend for S1
// Build: gcc S2.c -o S2
// Run:   ./S2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/types.h>

#define S2_PORT 6202
#define BACKLOG 16
#define BUFSZ   4096


static int cmp_cstr(const void *a, const void *b){
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

// >>> adjust if needed
static const char *ROOT = "/home/azeem7/S2";

static ssize_t write_n(int fd, const void *buf, size_t n){
    size_t off=0; const char *p=(const char*)buf;
    while(off<n){
        ssize_t w=write(fd,p+off,n-off);
        if(w<0){ if(errno==EINTR) continue; return -1; }
        if(w==0) return (ssize_t)off;
        off += (size_t)w;
    }
    return (ssize_t)off;
}
static ssize_t read_line(int fd, char *buf, size_t len){
    size_t i=0;
    while(i+1<len){
        char c; ssize_t r=read(fd,&c,1);
        if(r<0){ if(errno==EINTR) continue; return -1; }
        if(r==0) break;
        buf[i++]=c;
        if(c=='\n') break;
    }
    buf[i]='\0'; return (ssize_t)i;
}
static int ensure_dir(const char *path){
    char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",path);
    for(char *p=tmp+1; *p; ++p){
        if(*p=='/'){ *p='\0'; if(mkdir(tmp,0775)==-1 && errno!=EEXIST) return -1; *p='/'; }
    }
    if(mkdir(tmp,0775)==-1 && errno!=EEXIST) return -1;
    return 0;
}
static void join_path(char *out, size_t outsz, const char *root, const char *dest){
    if(!dest||!*dest) { snprintf(out,outsz,"%s",root); return; }
    if(dest[0]=='/') snprintf(out,outsz,"%s%s",root,dest);
    else snprintf(out,outsz,"%s/%s",root,dest);
}
// --- tar helper ---
static int make_tar_for_root(const char *root, const char *ext, char *outpath, size_t outsz){
    char tmp[]="/tmp/s2tarXXXXXX"; int fd=mkstemp(tmp); if(fd<0) return -1; close(fd);
    char pat[32]; snprintf(pat,sizeof(pat),"*%s",ext);
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "set -e; cd '%s'; "
        "FILES=$(find . -type f -name '%s' -print); "
        "if [ -z \"$FILES\" ]; then tar -cf '%s' --files-from /dev/null; "
        "else tar -cf '%s' $FILES; fi",
        root, pat, tmp, tmp);
    int rc=system(cmd);
    if(rc!=0){ unlink(tmp); return -2; }
    snprintf(outpath,outsz,"%s",tmp);
    return 0;
}

static void handle_client(int csd){
    char line[2048];
    while(1){
        ssize_t n=read_line(csd,line,sizeof(line)); if(n<=0) break;

        if(strncmp(line,"STORE ",6)==0){
            char dest[1024], fname[256]; long long size=0;
            if(sscanf(line+6,"%1023s %255s %lld",dest,fname,&size)!=3 || size<0){ dprintf(csd,"ERR bad STORE\n"); break; }
            if(strstr(dest,"..")){ dprintf(csd,"ERR badpath\n"); break; }
            char dpath[2048]; join_path(dpath,sizeof(dpath),ROOT,dest);
            if(ensure_dir(dpath)<0){ dprintf(csd,"ERR makedir\n"); break; }
            char full[3072]; snprintf(full,sizeof(full),"%s/%s",dpath,fname);
            int fd=open(full,O_CREAT|O_TRUNC|O_WRONLY,0664); if(fd<0){ dprintf(csd,"ERR open\n"); break; }
            char buf[BUFSZ]; long long left=size;
            while(left>0){
                ssize_t r=read(csd,buf,(left>BUFSZ?BUFSZ:(size_t)left));
                if(r<=0){ close(fd); unlink(full); dprintf(csd,"ERR stream\n"); return; }
                if(write_n(fd,buf,(size_t)r)!=r){ close(fd); unlink(full); dprintf(csd,"ERR disk\n"); return; }
                left-=r;
            }
            close(fd); dprintf(csd,"OK\n");
        }
        else if(strncmp(line,"FETCH ",6)==0){
            char dest[1024], fname[256];
            if(sscanf(line+6,"%1023s %255s",dest,fname)!=2){ dprintf(csd,"ERR bad FETCH\n"); break; }
            if(strstr(dest,"..")){ dprintf(csd,"ERR badpath\n"); break; }
            char dpath[2048]; join_path(dpath,sizeof(dpath),ROOT,dest);
            char full[3072]; snprintf(full,sizeof(full),"%s/%s",dpath,fname);
            int fd=open(full,O_RDONLY); if(fd<0){ dprintf(csd,"ERR nofile\n"); break; }
            struct stat st; fstat(fd,&st); long long size=st.st_size;
            dprintf(csd,"OK %lld\n",size);
            char buf[BUFSZ]; long long left=size;
            while(left>0){ ssize_t r=read(fd,buf,(left>BUFSZ?BUFSZ:(size_t)left)); if(r<=0) break;
                if(write_n(csd,buf,(size_t)r)!=r) break; left-=r; }
            close(fd);
        }
        else if(strncmp(line,"DELETE ",7)==0){
            char dest[1024], fname[256];
            if(sscanf(line+7,"%1023s %255s",dest,fname)!=2){ dprintf(csd,"ERR bad DELETE\n"); break; }
            if(strstr(dest,"..")){ dprintf(csd,"ERR badpath\n"); break; }
            char dpath[2048]; join_path(dpath,sizeof(dpath),ROOT,dest);
            char full[3072]; snprintf(full,sizeof(full),"%s/%s",dpath,fname);
            int rc=unlink(full); dprintf(csd, (rc==0)?"OK\n":"ERR\n");
        }
        else if(strncmp(line,"TARALL ",7)==0){
            char ext[16];
            if(sscanf(line+7,"%15s",ext)!=1){ dprintf(csd,"ERR bad TARALL\n"); break; }
            if(strcmp(ext,".pdf")!=0){ dprintf(csd,"ERR ext\n"); break; }
            char tarpath[256];
            if(make_tar_for_root(ROOT, ".pdf", tarpath, sizeof(tarpath))!=0){ dprintf(csd,"ERR tar\n"); break; }
            int fd=open(tarpath,O_RDONLY); if(fd<0){ unlink(tarpath); dprintf(csd,"ERR taropen\n"); break; }
            struct stat st; fstat(fd,&st);
            dprintf(csd,"OK %lld\n",(long long)st.st_size);
            char buf[BUFSZ]; ssize_t r;
            while((r=read(fd,buf,sizeof(buf)))>0) if(write_n(csd,buf,(size_t)r)!=r) break;
            close(fd); unlink(tarpath);
        }
          /* ---- LIST <dest> : return sorted names with this server's extension ---- */
else if (strncmp(line, "LIST ", 5) == 0) {
    char dest[1024];
    if (sscanf(line+5, "%1023s", dest) != 1) { dprintf(csd,"ERR bad LIST\n"); continue; }
    if (strstr(dest, "..")) { dprintf(csd,"ERR badpath\n"); continue; }

    char dir[2048];
    if (dest[0]=='/') snprintf(dir, sizeof(dir), "%s%s", ROOT, dest);
    else              snprintf(dir, sizeof(dir), "%s/%s", ROOT, dest);

    DIR *dp = opendir(dir);
    if (!dp) { dprintf(csd,"OK 0\n"); continue; }

    char *names[4096]; int n=0;
    struct dirent *de;
    while ((de=readdir(dp))) {
        if (de->d_name[0]=='.') continue;
        const char *dot = strrchr(de->d_name, '.');
        if (dot && strcasecmp(dot, ".pdf")==0) names[n++] = strdup(de->d_name);
        if (n>=4096) break;
    }
    closedir(dp);
    qsort(names, n, sizeof(char*), cmp_cstr);
    dprintf(csd, "OK %d\n", n);
    for (int i=0;i<n;i++){ dprintf(csd,"NAME %s\n", names[i]); free(names[i]); }
}
        else if(strncmp(line,"QUIT",4)==0) break;
        else dprintf(csd,"ERR unknown\n");
    }
    close(csd);
}

int main(void){
    int sd=socket(AF_INET,SOCK_STREAM,0); if(sd<0){ perror("socket"); return 1; }
    int opt=1; setsockopt(sd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY); a.sin_port=htons(S2_PORT);
    if(bind(sd,(struct sockaddr*)&a,sizeof(a))<0){ perror("bind"); return 1; }
    if(listen(sd,BACKLOG)<0){ perror("listen"); return 1; }
    fprintf(stderr,"S2 listening on %d, root=%s\n", S2_PORT, ROOT);
    while(1){
        int csd=accept(sd,NULL,NULL);
        if(csd<0){ if(errno==EINTR) continue; perror("accept"); break; }
        pid_t pid=fork();
        if(pid==0){ close(sd); handle_client(csd); _exit(0); }
        close(csd);
    }
    close(sd); return 0;
}
