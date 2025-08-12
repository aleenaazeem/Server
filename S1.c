// S1.c — Main server for COMP-8567 DFS project
// Features: uploadf, downlf, removef, downltar (talks to S2/S3/S4).
// Build: gcc S1.c -o S1
// Run:   ./S1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define BUFSZ   4096
#define BACKLOG 16

#define S1_PORT 6201
#define S2_PORT 6202
#define S3_PORT 6203
#define S4_PORT 6204

// >>>>>> CHANGE THIS to your actual path <<<<<<
static const char *S1_ROOT = "/home/azeem7/S1";

/* ---------- small I/O helpers ---------- */
static ssize_t write_n(int fd, const void *buf, size_t n){
    size_t off=0; const char *p=(const char*)buf;
    while(off<n){
        ssize_t w=write(fd, p+off, n-off);
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
    buf[i]='\0';
    return (ssize_t)i;
}

/* ---------- path helpers ---------- */
static int ensure_dir(const char *path){
    char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",path);
    for(char *p=tmp+1; *p; ++p){
        if(*p=='/'){
            *p='\0';
            if(mkdir(tmp,0775)==-1 && errno!=EEXIST) return -1;
            *p='/';
        }
    }
    if(mkdir(tmp,0775)==-1 && errno!=EEXIST) return -1;
    return 0;
}
static void join_path(char *out, size_t outsz, const char *root, const char *dest){
    if(!dest || !*dest) { snprintf(out,outsz,"%s",root); return; }
    if(dest[0]=='/') snprintf(out,outsz,"%s%s",root,dest);
    else snprintf(out,outsz,"%s/%s",root,dest);
}
static const char* file_ext(const char *name){
    const char *dot = strrchr(name, '.');
    return (!dot || dot==name) ? "" : dot;
}

/* ---------- sockets to S2/S3/S4 ---------- */
static int connect_local_port(int port){
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if(sd<0) return -1;
    struct sockaddr_in a={0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    if(connect(sd,(struct sockaddr*)&a,sizeof(a))<0){ close(sd); return -1; }
    return sd;
}

/* ---------- upload forwarding (.pdf/.txt/.zip) ---------- */
static int forward_store_file(int port, const char *dest, const char *fname,
                              const char *local_fullpath, long long size){
    int in_fd = open(local_fullpath, O_RDONLY);
    if(in_fd < 0) return -1;

    int sd = connect_local_port(port);
    if(sd < 0){ close(in_fd); return -2; }

    dprintf(sd, "STORE %s %s %lld\n", dest, fname, size);

    char buf[BUFSZ]; long long left = size;
    while(left > 0){
        ssize_t r = read(in_fd, buf, (left > BUFSZ ? BUFSZ : (size_t)left));
        if(r <= 0){ close(in_fd); close(sd); return -3; }
        if(write_n(sd, buf, (size_t)r) != r){ close(in_fd); close(sd); return -4; }
        left -= r;
    }
    close(in_fd);

    char line[256]; ssize_t rn = read_line(sd, line, sizeof(line));
    close(sd);
    if(rn <= 0 || strncmp(line,"OK",2)!=0) return -5;

    // success: remove from S1 (client is unaware)
    unlink(local_fullpath);
    return 0;
}

/* ---------- download helpers ---------- */
static int stream_local_file(int out, const char *absdir, const char *fname){
    char full[3072]; snprintf(full,sizeof(full), "%s/%s", absdir, fname);
    int fd = open(full, O_RDONLY);
    if(fd < 0) return -1;

    struct stat st; fstat(fd, &st);
    dprintf(out, "FILE %s %lld\n", fname, (long long)st.st_size);

    char buf[BUFSZ]; ssize_t r;
    while((r = read(fd, buf, sizeof(buf))) > 0){
        if(write_n(out, buf, (size_t)r) != r){ close(fd); return -2; }
    }
    close(fd);
    return 0;
}
static int relay_from_aux(int out, int port, const char *dest, const char *fname){
    int sd = connect_local_port(port);
    if(sd < 0) return -1;
    dprintf(sd, "FETCH %s %s\n", dest, fname);

    char hdr[256]; ssize_t rn = read_line(sd, hdr, sizeof(hdr));
    if(rn <= 0 || strncmp(hdr, "OK ", 3) != 0){ close(sd); return -2; }
    long long size=0; if(sscanf(hdr+3, "%lld", &size)!=1 || size<0){ close(sd); return -3; }

    dprintf(out, "FILE %s %lld\n", fname, size);

    char buf[BUFSZ]; long long left = size;
    while(left > 0){
        ssize_t r = read(sd, buf, (left > BUFSZ ? BUFSZ : (size_t)left));
        if(r <= 0){ close(sd); return -4; }
        if(write_n(out, buf, (size_t)r) != r){ close(sd); return -5; }
        left -= r;
    }
    close(sd);
    return 0;
}

/* ---------- remove helpers ---------- */
static int delete_local(const char *dest, const char *fname){
    char dir[2048]; join_path(dir,sizeof(dir),S1_ROOT,dest);
    char full[3072]; snprintf(full,sizeof(full), "%s/%s", dir, fname);
    return unlink(full); // 0 on success
}
static int delete_remote(int port, const char *dest, const char *fname){
    int sd = connect_local_port(port);
    if(sd < 0) return -1;
    dprintf(sd, "DELETE %s %s\n", dest, fname);
    char line[128]; ssize_t rn = read_line(sd, line, sizeof(line));
    close(sd);
    if(rn <= 0) return -2;
    return (strncmp(line,"OK",2)==0) ? 0 : -3;
}

/* ---------- tar helpers (downltar) — robust `.c` path ---------- */
static int ends_with_ext(const char *name, const char *ext){
    size_t ln=strlen(name), le=strlen(ext);
    if(le==0 || ln<le) return 0;
    return strcasecmp(name+ln-le, ext)==0;
}

// Recursively collect relative file paths ending with 'ext' into 'outf'.
// root = absolute root; rel = current relative path from root ("" for top)
static int collect_rec(const char *root, const char *rel, const char *ext, FILE *outf, int *count){
    char abspath[4096];
    if(rel && *rel) snprintf(abspath,sizeof(abspath), "%s/%s", root, rel);
    else            snprintf(abspath,sizeof(abspath), "%s", root);

    DIR *dp = opendir(abspath);
    if(!dp) return 0; // not fatal

    struct dirent *de;
    while((de=readdir(dp))){
        if(de->d_name[0]=='.') continue;
        char next_rel[4096];
        if(rel && *rel) snprintf(next_rel,sizeof(next_rel), "%s/%s", rel, de->d_name);
        else            snprintf(next_rel,sizeof(next_rel), "%s", de->d_name);

        char next_abs[4096]; snprintf(next_abs,sizeof(next_abs), "%s/%s", root, next_rel);

        struct stat st;
        if(lstat(next_abs, &st)==0){
            if(S_ISDIR(st.st_mode)){
                collect_rec(root, next_rel, ext, outf, count);
            }else if(S_ISREG(st.st_mode)){
                if(ends_with_ext(de->d_name, ext)){
                    fprintf(outf, "%s\n", next_rel); // relative path only
                    (*count)++;
                }
            }
        }
    }
    closedir(dp);
    return 0;
}

// Create a tar with all files under 'root' that end with 'ext'.
// On success, writes the tmp tar path into outpath and returns 0.
static int make_tar_for_root(const char *root, const char *ext, char *outpath, size_t outsz){
    char listtmp[] = "/tmp/s1listXXXXXX";
    char tartmp[]  = "/tmp/s1tarXXXXXX";
    int lfd = mkstemp(listtmp);
    int tfd = mkstemp(tartmp);
    if(lfd<0 || tfd<0){
        if(lfd>=0) { close(lfd); unlink(listtmp); }
        if(tfd>=0) { close(tfd); unlink(tartmp); }
        return -1;
    }
    close(tfd); // tar will recreate it
    FILE *lfp = fdopen(lfd, "w");
    if(!lfp){ close(lfd); unlink(listtmp); unlink(tartmp); return -1; }

    int count = 0;
    collect_rec(root, "", ext, lfp, &count);
    fflush(lfp); fsync(lfd); fclose(lfp);

    pid_t pid = fork();
    if(pid<0){
        unlink(listtmp); unlink(tartmp);
        return -1;
    }
    if(pid==0){
        if(count==0){
            // empty archive is valid
            execlp("tar", "tar", "-cf", tartmp, "--files-from", "/dev/null", (char*)NULL);
        }else{
            execlp("tar", "tar", "-C", root, "-cf", tartmp, "-T", listtmp, (char*)NULL);
        }
        _exit(127);
    }
    int status=0; waitpid(pid, &status, 0);
    unlink(listtmp);
    if(!WIFEXITED(status) || WEXITSTATUS(status)!=0){
        unlink(tartmp);
        return -2;
    }
    snprintf(outpath, outsz, "%s", tartmp);
    return 0;
}

// fetch tar stream from S2/S3 into 'out_fd' and return size, or <0 on error
static long long fetch_tar_from_aux(int port, const char *ext, int out_fd){
    int sd = connect_local_port(port);
    if(sd < 0) return -1;
    dprintf(sd, "TARALL %s\n", ext);            // ext = ".pdf" or ".txt"

    char hdr[256]; ssize_t rn = read_line(sd, hdr, sizeof(hdr));
    if(rn <= 0 || strncmp(hdr, "OK ", 3) != 0){ close(sd); return -2; }
    long long size=0; if(sscanf(hdr+3,"%lld",&size)!=1 || size<0){ close(sd); return -3; }

    char buf[BUFSZ]; long long left=size;
    while(left>0){
        ssize_t r = read(sd, buf, (left > BUFSZ ? BUFSZ : (size_t)left));
        if(r <= 0){ close(sd); return -4; }
        if(write_n(out_fd, buf, (size_t)r) != r){ close(sd); return -5; }
        left -= r;
    }
    close(sd);
    return size;
}
/*---------------------------------------------------------------*/
// list local files under S1_ROOT/dest with a given extension; returns sorted array
static int s1_list_local_by_ext(const char *dest, const char *ext, char ***out_names){
    char dir[2048]; join_path(dir, sizeof(dir), S1_ROOT, dest);
    DIR *dp = opendir(dir);
    if(!dp){ *out_names=NULL; return 0; }

    int n=0, cap=32;
    char **names = malloc(cap * sizeof(char*));
    struct dirent *de;
    while((de=readdir(dp))){
        if(de->d_name[0]=='.') continue;
        const char *dot = strrchr(de->d_name,'.');
        if(dot && strcasecmp(dot, ext)==0){
            if(n==cap){ cap*=2; names = realloc(names, cap*sizeof(char*)); }
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(dp);

    int cmpstr(const void *a, const void *b){ return strcmp(*(char *const*)a, *(char *const*)b); }
    if(n>0) qsort(names, n, sizeof(char*), cmpstr);
    *out_names = names;
    return n;
}

// ask an auxiliary server to LIST; returns count and malloc'd array (sorted by server)
static int s1_request_list_from_aux(int port, const char *dest, char ***out_names){
    int sd = connect_local_port(port);
    if(sd < 0){ *out_names=NULL; return -1; }

    dprintf(sd, "LIST %s\n", dest);

    char hdr[256];
    if(read_line(sd, hdr, sizeof(hdr)) <= 0 || strncmp(hdr,"OK ",3)!=0){ close(sd); *out_names=NULL; return -2; }

    int count=0; sscanf(hdr+3, "%d", &count);
    if(count <= 0){ close(sd); *out_names=NULL; return 0; }

    char **names = malloc((size_t)count * sizeof(char*));
    for(int i=0;i<count;i++){
        char ln[512];
        if(read_line(sd, ln, sizeof(ln)) <= 0 || strncmp(ln,"NAME ",5)!=0){
            count = i; break;
        }
        char nm[256]; sscanf(ln+5, "%255s", nm);
        names[i] = strdup(nm);
    }
    close(sd);
    *out_names = names;
    return count;
}

/* ---------- per-client handler (prcclient) ---------- */
static void prcclient(int csd){
    char line[2048];

    while(1){
        ssize_t n = read_line(csd, line, sizeof(line));
        if(n <= 0) break;

        /* ===== UPLOAD ===== */
        if(strncmp(line, "UPLOAD ", 7) == 0){
            int nfiles=0; char dest[1024];
            if(sscanf(line+7, "%d %1023s", &nfiles, dest) != 2 || nfiles <= 0 || nfiles > 3){
                dprintf(csd, "ERR bad UPLOAD\n"); continue;
            }
            if(strncmp(dest, "~S1/", 4) == 0) memmove(dest, dest+3, strlen(dest+3)+1);
            if(strstr(dest, "..")){ dprintf(csd, "ERR badpath\n"); continue; }

            char s1_dest[2048]; join_path(s1_dest, sizeof(s1_dest), S1_ROOT, dest);
            if(ensure_dir(s1_dest) < 0){ dprintf(csd, "ERR makedir\n"); continue; }

            for(int i=0;i<nfiles;i++){
                char nline[1024], sline[1024];
                if(read_line(csd, nline, sizeof(nline)) <= 0){ dprintf(csd,"ERR name\n"); return; }
                if(strncmp(nline, "NAME ", 5) != 0){ dprintf(csd,"ERR namehdr\n"); return; }
                char fname[256]; if(sscanf(nline+5, "%255s", fname) != 1){ dprintf(csd,"ERR nameparse\n"); return; }

                if(read_line(csd, sline, sizeof(sline)) <= 0){ dprintf(csd,"ERR size\n"); return; }
                if(strncmp(sline, "SIZE ", 5) != 0){ dprintf(csd,"ERR sizehdr\n"); return; }
                long long fbytes=0; if(sscanf(sline+5, "%lld", &fbytes) != 1 || fbytes < 0){ dprintf(csd,"ERR sizeparse\n"); return; }

                char full_local[3072]; snprintf(full_local,sizeof(full_local), "%s/%s", s1_dest, fname);
                int fd = open(full_local, O_CREAT|O_TRUNC|O_WRONLY, 0664);
                if(fd < 0){ dprintf(csd, "ERR open\n"); return; }

                long long left=fbytes; char buf[BUFSZ];
                while(left > 0){
                    ssize_t r=read(csd, buf, (left>BUFSZ?BUFSZ:(size_t)left));
                    if(r <= 0){ close(fd); unlink(full_local); dprintf(csd,"ERR stream\n"); return; }
                    if(write_n(fd, buf, (size_t)r) != r){ close(fd); unlink(full_local); dprintf(csd,"ERR disk\n"); return; }
                    left -= r;
                }
                fsync(fd); close(fd);

                // route non-.c in the background
                const char *ext = file_ext(fname);
                int fport = 0;
                if(!strcasecmp(ext, ".pdf")) fport = S2_PORT;
                else if(!strcasecmp(ext, ".txt")) fport = S3_PORT;
                else if(!strcasecmp(ext, ".zip")) fport = S4_PORT;

                if(fport){
                    pid_t wp = fork();
                    if(wp == 0){
                        (void)forward_store_file(fport, dest, fname, full_local, fbytes);
                        _exit(0);
                    }
                }
            }
            dprintf(csd, "OK\n");
        }

        /* ===== DOWNLF ===== */
        else if(strncmp(line, "DOWNLF ", 7) == 0){
            int nreq=0; if(sscanf(line+7, "%d", &nreq) != 1 || nreq<=0 || nreq>2){ dprintf(csd,"ERR bad DOWNLF\n"); continue; }
            for(int i=0;i<nreq;i++){
                char pline[1200]; if(read_line(csd, pline, sizeof(pline)) <= 0){ dprintf(csd,"ERR path\n"); return; }
                if(strncmp(pline,"PATH ",5)!=0){ dprintf(csd,"ERR pathhdr\n"); return; }
                char full[1024]; if(sscanf(pline+5,"%1023s", full) != 1){ dprintf(csd,"ERR pathparse\n"); return; }

                if(strncmp(full,"~S1/",4)==0) memmove(full, full+3, strlen(full+3)+1);
                if(strstr(full,"..")){ dprintf(csd,"ERR badpath\n"); return; }

                char *slash = strrchr(full, '/');
                if(!slash || slash==full){ dprintf(csd,"ERR badname\n"); return; }
                char dest[1024], fname[256];
                size_t dlen=(size_t)(slash - full);
                snprintf(dest,sizeof(dest), "%.*s", (int)dlen, full);
                snprintf(fname,sizeof(fname), "%s", slash+1);

                const char *ext = file_ext(fname);
                if(!strcasecmp(ext, ".c")){
                    char absdir[2048]; join_path(absdir, sizeof(absdir), S1_ROOT, dest);
                    if(stream_local_file(csd, absdir, fname) != 0) dprintf(csd,"ERR nofile %s\n",fname);
                }else{
                    int port = (!strcasecmp(ext,".pdf"))?S2_PORT:(!strcasecmp(ext,".txt"))?S3_PORT:(!strcasecmp(ext,".zip"))?S4_PORT:0;
                    if(!port){ dprintf(csd,"ERR type %s\n",fname); continue; }
                    if(relay_from_aux(csd, port, dest, fname) != 0) dprintf(csd,"ERR fetch %s\n",fname);
                }
            }
        }

        /* ===== REMOVEF ===== */
        else if(strncmp(line, "REMOVEF ", 8) == 0){
            int nreq=0; if(sscanf(line+8,"%d",&nreq)!=1 || nreq<=0 || nreq>2){ dprintf(csd,"ERR bad REMOVEF\n"); continue; }
            for(int i=0;i<nreq;i++){
                char pline[1200]; if(read_line(csd, pline, sizeof(pline)) <= 0){ dprintf(csd,"ERR path\n"); return; }
                if(strncmp(pline,"PATH ",5)!=0){ dprintf(csd,"ERR pathhdr\n"); return; }
                char full[1024]; if(sscanf(pline+5,"%1023s",full)!=1){ dprintf(csd,"ERR pathparse\n"); return; }

                if(strncmp(full,"~S1/",4)==0) memmove(full, full+3, strlen(full+3)+1);
                if(strstr(full,"..")){ dprintf(csd,"ERR badpath\n"); return; }

                char *slash=strrchr(full,'/'); if(!slash||slash==full){ dprintf(csd,"ERR badname\n"); return; }
                char dest[1024], fname[256];
                size_t dlen=(size_t)(slash-full);
                snprintf(dest,sizeof(dest), "%.*s", (int)dlen, full);
                snprintf(fname,sizeof(fname), "%s", slash+1);

                const char *ext=file_ext(fname);
                int rc=-1;
                if(!strcasecmp(ext,".c")) rc = delete_local(dest,fname);
                else{
                    int port = (!strcasecmp(ext,".pdf"))?S2_PORT:(!strcasecmp(ext,".txt"))?S3_PORT:(!strcasecmp(ext,".zip"))?S4_PORT:0;
                    if(port==0) rc = delete_local(dest,fname);
                    else rc = delete_remote(port, dest, fname);
                }
                if(rc==0) dprintf(csd,"OK %s\n",fname);
                else      dprintf(csd,"ERR %s\n",fname);
            }
        }

        /* ===== DOWNLTAR ===== */
        else if(strncmp(line, "DOWNLTAR ", 9) == 0){
            char ext[16]; if(sscanf(line+9,"%15s",ext)!=1){ dprintf(csd,"ERR bad DOWNLTAR\n"); continue; }
            if(strcmp(ext,".c") && strcmp(ext,".pdf") && strcmp(ext,".txt")){ dprintf(csd,"ERR ext\n"); continue; }

            if(strcmp(ext,".c")==0){
                char tarpath[256];
                if(make_tar_for_root(S1_ROOT, ".c", tarpath, sizeof(tarpath)) != 0){ dprintf(csd,"ERR tar\n"); continue; }
                int fd=open(tarpath,O_RDONLY);
                if(fd<0){ unlink(tarpath); dprintf(csd,"ERR taropen\n"); continue; }
                struct stat st; fstat(fd,&st);
                dprintf(csd,"TAR cfiles.tar %lld\n",(long long)st.st_size);
                char buf[BUFSZ]; ssize_t r; while((r=read(fd,buf,sizeof(buf)))>0) if(write_n(csd,buf,(size_t)r)!=r) break;
                close(fd); unlink(tarpath);
            }else{
                int port = (strcmp(ext,".pdf")==0)?S2_PORT:S3_PORT;
                char tmp[]="/tmp/s1relayXXXXXX"; int fd=mkstemp(tmp);
                if(fd<0){ dprintf(csd,"ERR tmp\n"); continue; }
                long long sz = fetch_tar_from_aux(port, ext, fd);
                if(sz < 0){ close(fd); unlink(tmp); dprintf(csd,"ERR fetch\n"); continue; }
                fsync(fd); lseek(fd,0,SEEK_SET);
                const char *tname = (strcmp(ext,".pdf")==0) ? "pdf.tar" : "text.tar";
                dprintf(csd,"TAR %s %lld\n", tname, sz);
                char buf[BUFSZ]; ssize_t r; while((r=read(fd,buf,sizeof(buf)))>0) if(write_n(csd,buf,(size_t)r)!=r) break;
                close(fd); unlink(tmp);
            }
        }
         /* ===== DISPFNAMES =====
   Syntax from client: DISPFNAMES <~S1/path>
   Response: NAMES <total>\n followed by 'NAME <file>\n' lines
         */
else if (strncmp(line, "DISPFNAMES ", 11) == 0) {
    char path[1024];
    if (sscanf(line+11, "%1023s", path) != 1) { dprintf(csd,"ERR bad DISPFNAMES\n"); continue; }

    // Normalize ~S1, supporting both "~S1" and "~S1/<subdir>"
    if (strncmp(path, "~S1", 3) == 0) {
        if (path[3] == '/') {                     // "~S1/<something>"
            memmove(path, path+3, strlen(path+3)+1);   // becomes "/<something>"
        } else if (path[3] == '\0') {             // exactly "~S1"
            strcpy(path, "/");                    // treat as root
        }
    }
    if (strstr(path, "..")) { dprintf(csd,"ERR badpath\n"); continue; }


    // gather per-type (order must be: .c, .pdf, .txt, .zip)
    char **cN=NULL, **pdfN=NULL, **txtN=NULL, **zipN=NULL;
    int nC   = s1_list_local_by_ext(path, ".c",   &cN);
    int nPDF = s1_request_list_from_aux(S2_PORT, path, &pdfN); if(nPDF<0) nPDF=0;
    int nTXT = s1_request_list_from_aux(S3_PORT, path, &txtN); if(nTXT<0) nTXT=0;
    int nZIP = s1_request_list_from_aux(S4_PORT, path, &zipN); if(nZIP<0) nZIP=0;

    int total = nC + nPDF + nTXT + nZIP;
    dprintf(csd, "NAMES %d\n", total);

    for(int i=0;i<nC;i++){  dprintf(csd,"NAME %s\n", cN[i]);  free(cN[i]); }   free(cN);
    for(int i=0;i<nPDF;i++){dprintf(csd,"NAME %s\n", pdfN[i]); free(pdfN[i]); } free(pdfN);
    for(int i=0;i<nTXT;i++){dprintf(csd,"NAME %s\n", txtN[i]); free(txtN[i]); } free(txtN);
    for(int i=0;i<nZIP;i++){dprintf(csd,"NAME %s\n", zipN[i]); free(zipN[i]); } free(zipN);
}

        /* ===== QUIT / unknown ===== */
        else if(strncmp(line,"QUIT",4)==0){ break; }
        else dprintf(csd, "ERR unknown\n");
    }

    close(csd);
}

/* ---------- main: accept + fork per client ---------- */
int main(void){
    signal(SIGCHLD, SIG_IGN); // avoid zombies

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if(sd<0){ perror("socket"); return 1; }
    int opt=1; setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in a={0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(S1_PORT);

    if(bind(sd,(struct sockaddr*)&a,sizeof(a))<0){ perror("bind"); return 1; }
    if(listen(sd, BACKLOG)<0){ perror("listen"); return 1; }

    fprintf(stderr, "S1 listening on %d, root=%s\n", S1_PORT, S1_ROOT);

    while(1){
        int csd = accept(sd, NULL, NULL);
        if(csd < 0){ if(errno==EINTR) continue; perror("accept"); break; }
        pid_t pid = fork();
        if(pid == 0){
            close(sd);
            prcclient(csd);
            _exit(0);
        }
        close(csd);
    }
    close(sd);
    return 0;
}
