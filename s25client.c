// s25client.c — matches your S1 protocol exactly.
// Commands:
//   uploadf <f1> [f2] [f3] <dest>
//   downlf  <~S1/path/file1> [~S1/path/file2]
//   removef <~S1/path/file1> [~S1/path/file2]
//   downltar .c|.pdf|.txt
//   quit
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define S1_PORT 6201
#define BUFSZ   4096

static void usage(){
    fprintf(stderr,
        "Commands:\n"
        "  uploadf <f1> [f2] [f3] <dest>\n"
        "  downlf  <~S1/path/file1> [~S1/path/file2]\n"
        "  removef <~S1/path/file1> [~S1/path/file2]\n"
        "  downltar .c|.pdf|.txt\n"
        "  quit\n");
}
static off_t file_size(const char *p){ struct stat st; if(stat(p,&st)==0) return st.st_size; return -1; }
static const char* base_name(const char *p){ const char *s=strrchr(p,'/'); return s? s+1 : p; }

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
    buf[i]='\0';
    return (ssize_t)i;
}

int main(){
    int sd=socket(AF_INET,SOCK_STREAM,0); if(sd<0){ perror("socket"); return 1; }
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(S1_PORT); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(sd,(struct sockaddr*)&a,sizeof(a))<0){ perror("connect"); return 1; }
    fprintf(stderr,"Connected to S1:%d\n",S1_PORT);

    char line[2048];
    while(1){
        fprintf(stderr,"s25client$ ");
        if(!fgets(line,sizeof(line),stdin)) break;
        line[strcspn(line,"\n")] = 0;
        if(!*line) continue;

        if(!strncmp(line,"quit",4)){ dprintf(sd,"QUIT\n"); break; }

        else if(!strncmp(line,"uploadf ",8)){
            char *args[6]; int argc=0; char *tok=strtok(line+8," ");
            while(tok && argc<5){ args[argc++]=tok; tok=strtok(NULL," "); }
            if(argc<2){ usage(); continue; }
            char *dest=args[argc-1]; int nfiles=argc-1; if(nfiles<1||nfiles>3){ usage(); continue; }

            off_t sizes[3]; const char *paths[3]; const char *names[3];
            for(int i=0;i<nfiles;i++){
                paths[i]=args[i]; sizes[i]=file_size(paths[i]);
                if(sizes[i]<0){ fprintf(stderr,"No such file: %s\n",paths[i]); goto next; }
                names[i]=base_name(paths[i]);
            }

            dprintf(sd,"UPLOAD %d %s\n",nfiles,dest);
            for(int i=0;i<nfiles;i++){
                dprintf(sd,"NAME %s\n",names[i]);
                dprintf(sd,"SIZE %lld\n",(long long)sizes[i]);
                int fd=open(paths[i],O_RDONLY); if(fd<0){ perror("open"); goto next; }
                char buf[BUFSZ]; off_t left=sizes[i];
                while(left>0){
                    ssize_t r=read(fd,buf,(left>BUFSZ?BUFSZ:(size_t)left));
                    if(r<=0){ perror("read"); close(fd); goto next; }
                    if(write_n(sd,buf,(size_t)r)!=r){ perror("write"); close(fd); goto next; }
                    left-=r;
                }
                close(fd);
            }
            { char resp[256]; if(read_line(sd,resp,sizeof(resp))>0) fprintf(stderr,"S1: %s",resp); }
        }

        else if(!strncmp(line,"downlf ",7)){
            char *p1=strtok(line+7," "); char *p2=strtok(NULL," ");
            if(!p1){ usage(); continue; }
            int n=p2?2:1;
            dprintf(sd,"DOWNLF %d\n",n);
            dprintf(sd,"PATH %s\n",p1);
            if(p2) dprintf(sd,"PATH %s\n",p2);

            for(int i=0;i<n;i++){
                char hdr[256]; if(read_line(sd,hdr,sizeof(hdr))<=0){ fprintf(stderr,"Disconnected\n"); break; }
                if(strncmp(hdr,"FILE ",5)!=0){ fprintf(stderr,"%s",hdr); break; }
                char name[256]; long long size=0; if(sscanf(hdr+5,"%255s %lld",name,&size)!=2 || size<0){ fprintf(stderr,"Bad header\n"); break; }
                int fd=open(name,O_CREAT|O_TRUNC|O_WRONLY,0664); if(fd<0){ perror("open"); break; }
                char buf[BUFSZ]; long long left=size;
                while(left>0){
                    ssize_t r=read(sd,buf,(left>BUFSZ?BUFSZ:(size_t)left)); if(r<=0){ fprintf(stderr,"Stream ended early\n"); break; }
                    if(write_n(fd,buf,(size_t)r)!=r){ perror("write"); break; }
                    left-=r;
                }
                close(fd);
                fprintf(stderr,"Downloaded %s (%lld bytes)\n",name,size);
            }
        }

        else if(!strncmp(line,"removef ",8)){
            char *p1=strtok(line+8," "); char *p2=strtok(NULL," "); if(!p1){ usage(); continue; }
            int n=p2?2:1;
            dprintf(sd,"REMOVEF %d\n",n);
            dprintf(sd,"PATH %s\n",p1);
            if(p2) dprintf(sd,"PATH %s\n",p2);
            for(int i=0;i<n;i++){ char resp[256]; if(read_line(sd,resp,sizeof(resp))>0) fprintf(stderr,"%s",resp); }
        }

        else if(!strncmp(line,"downltar ",9)){
            char ext[16]; if(sscanf(line+9,"%15s",ext)!=1){ usage(); continue; }
            dprintf(sd,"DOWNLTAR %s\n",ext);

            char hdr[256]; if(read_line(sd,hdr,sizeof(hdr))<=0){ fprintf(stderr,"Disconnected\n"); break; }
            if(strncmp(hdr,"TAR ",4)!=0){ fprintf(stderr,"%s",hdr); continue; }
            char tname[64]; long long size=0; if(sscanf(hdr+4,"%63s %lld",tname,&size)!=2 || size<0){ fprintf(stderr,"Bad TAR header\n"); continue; }

            int fd=open(tname,O_CREAT|O_TRUNC|O_WRONLY,0664); if(fd<0){ perror("open"); continue; }
            char buf[BUFSZ]; long long left=size;
            while(left>0){
                ssize_t r=read(sd,buf,(left>BUFSZ?BUFSZ:(size_t)left)); if(r<=0) break;
                if(write_n(fd,buf,(size_t)r)!=r){ perror("write"); break; }
                left-=r;
            }
            close(fd);
            fprintf(stderr,"Downloaded %s (%lld bytes)\n",tname,size);
        }

        else usage();
        next: ;
    }

    close(sd); return 0;
}

