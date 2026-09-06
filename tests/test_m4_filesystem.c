#define _GNU_SOURCE
#include "m4.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

int cpc_frame_count;
void leds_ping_m4_disk(void) {}
void leds_ping_m4_net(void) {}
static M4 board;
static Mem memory;
static void put16(u8 *p, u16 n) { p[0]=n; p[1]=n>>8; }
static void put32(u8 *p, u32 n) { put16(p,n); put16(p+2,n>>16); }
static u32 get32(const u8 *p) { return p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }

static const u8 *command(u16 cmd, const void *args, size_t len) {
    m4_dataport_write(&board, (u8)(len+2));
    m4_dataport_write(&board, (u8)cmd);
    m4_dataport_write(&board, cmd>>8);
    for (size_t i=0;i<len;i++) m4_dataport_write(&board, ((const u8 *)args)[i]);
    m4_ackport_write(&board, &memory);
    assert(board.bus_mem[1]==(u8)cmd && board.bus_mem[2]==cmd>>8);
    return board.bus_mem+3;
}
static const u8 *path_command(u16 cmd, const char *path) { return command(cmd,path,strlen(path)+1); }

static void entry(u8 *p, const char *name, u8 attr, u16 cluster, u32 size) {
    memcpy(p,name,11); p[11]=attr; put16(p+26,cluster); put32(p+28,size);
    put16(p+24,0x5D25); put16(p+22,0x645C);
}
static void lfn_slot(u8 *p, unsigned seq, const char *name, u8 checksum) {
    static const int offsets[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    memset(p,0xFF,32); p[0]=seq; p[11]=15; p[12]=0; p[13]=checksum; p[26]=p[27]=0;
    unsigned start=((seq&31)-1)*13;
    for (int i=0;i<13;i++) put16(p+offsets[i],start+i<strlen(name)?(u8)name[start+i]:start+i==strlen(name)?0:0xFFFF);
}
static void image_fixture(const char *path) {
    FILE *f=fopen(path,"w+b"); assert(f);
    assert(!ftruncate(fileno(f),6000*512));
    u8 sector[512]={0}; sector[0]=0xEB; sector[2]=0x90;
    put16(sector+11,512); sector[13]=1; put16(sector+14,1); sector[16]=1;
    put16(sector+17,64); put16(sector+19,6000); put16(sector+22,24);
    sector[510]=0x55; sector[511]=0xAA; assert(fwrite(sector,512,1,f)==1);
    memset(sector,0,512); put16(sector,0xFFF8); put16(sector+2,0xFFFF); put16(sector+4,0xFFFF);
    for (unsigned c=3;c<=148;c++) put16(sector+c*2,c==148?0xFFFF:c+1);
    assert(fwrite(sector,512,1,f)==1);
    memset(sector,0,512); entry(sector,"EIGHTCHR   ",0x10,2,0);
    entry(sector+32,"BIG     BIN",0x27,3,0x12345);
    u8 sum=0; const char *alias="LONGNA~1TXT";
    for (int i=0;i<11;i++) sum=(u8)(((sum&1)<<7)+(sum>>1)+(u8)alias[i]);
    lfn_slot(sector+64,0x42,"Long named file.txt",sum);
    lfn_slot(sector+96,1,"Long named file.txt",sum);
    entry(sector+128,alias,0x20,0,0);
    /* Sequence zero used to cause a negative-index write in the LFN parser. */
    memset(sector+160,0xFF,32); sector[160]=0; sector[171]=15;
    sector[160]=0x40; /* not end-of-directory; invalid sequence 0 */
    entry(sector+192,"BADLFN  TXT",0x20,0,0);
    assert(!fseek(f,25*512,SEEK_SET)); assert(fwrite(sector,512,1,f)==1);
    fclose(f);
}
static void image_tests(const char *path) {
    m4_init(&board,""); m4_set_image(&board,path); assert(board.image_mounted);
    const u8 *p=path_command(0x4316,"/big.bin");
    assert(!p[0] && get32(p+1)==0x12345 && p[9]==0x27);
    assert(p[5]==0x25 && p[6]==0x5D && p[7]==0x5C && p[8]==0x64);
    assert(!strcmp((const char *)p+10,"BIG.BIN"));
    p=path_command(0x4316,"EIGHTCHR"); assert(!p[0] && p[9]==0x10);
    p=path_command(0x4316,"Long named file.txt");
    assert(!p[0] && !strcmp((const char *)p+10,"LONGNA~1.TXT"));
    assert(!strcmp((const char *)p+23,"Long named file.txt"));
    p=path_command(0x4316,"LONGNA~1.TXT"); assert(!p[0] && !strcmp((const char *)p+23,"Long named file.txt"));
    p=path_command(0x4316,"BADLFN.TXT"); assert(!p[0] && !strcmp((const char *)p+23,"BADLFN.TXT"));
    p=path_command(0x4316,"ABSENT.BIN"); assert(p[0]==M4_ERR_NOFILE);
    p=command(0x4316,"BIG.BIN",7); assert(p[0]==M4_ERR_BADNAME);
    p=command(0x4316,NULL,0); assert(p[0]==M4_ERR_BADNAME);
    p=path_command(0x4316,""); assert(p[0]==M4_ERR_BADNAME);
    path_command(0x4325,""); p=command(0x4306,NULL,0);
    assert(board.bus_mem[0]==22 && !memcmp(p,">EIGHTCH.   ",12));
    path_command(0x4325,""); u8 limit=64;
    p=command(0x4306,&limit,1); assert(!strcmp((const char *)p,">EIGHTCHR"));
    /* Stat lookup must not consume or restart the live enumerator. */
    path_command(0x4316,"EIGHTCHR");
    p=command(0x4306,&limit,1); assert(!strcmp((const char *)p,"BIG.BIN"));
    assert(!strcmp((const char *)p+8,"74565"));
    p=command(0x4306,&limit,1); assert(!strcmp((const char *)p,"Long named file.txt"));
    p=command(0x4306,&limit,1); assert(!strcmp((const char *)p,"BADLFN.TXT"));
    command(0x4306,&limit,1); assert(board.bus_mem[0]==2);
    path_command(0x4325,""); limit=3;
    p=command(0x4306,&limit,1); assert(!strcmp((const char *)p,">EIG"));
    limit=0; p=command(0x4306,&limit,1); assert(p[0]==M4_ERR_BADNAME);
    m4_set_image(&board,""); m4_reset(&board);
}
static const u8 *open_file(u8 mode, const char *name) {
    u8 args[253]; size_t n=strlen(name)+1; assert(n+1<=sizeof(args));
    args[0]=mode; memcpy(args+1,name,n); return command(0x4301,args,n+1);
}
static void close_file(u8 fd) { assert(!command(0x4304,&fd,1)[0]); }
static void no_files(void) {
    for (unsigned i=0;i<M4_MAX_FDS;i++) assert(!board.fds[i].in_use);
}
static u8 *image_bytes(const char *path, size_t *size) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);assert(n>0);*size=(size_t)n;rewind(f);
    u8 *data=malloc(*size);assert(data);assert(fread(data,1,*size,f)==*size);fclose(f);
    return data;
}
static void unchanged_image(const char *path,const u8 *before,size_t size) {
    size_t after_size;u8 *after=image_bytes(path,&after_size);
    assert(after_size==size && !memcmp(before,after,size));free(after);
}
static void file_contents(const char *name,const char *expected,unsigned count) {
    FatFile *f=fat_open(&board.image_vol,name,false);assert(f);
    char bytes[32];assert(count<sizeof(bytes));
    assert(fat_file_size(f)==count && fat_read(f,bytes,sizeof(bytes))==count);
    assert(!memcmp(bytes,expected,count));fat_close(f);
}
static void write_fixture(const char *path) {
    image_fixture(path);
    FILE *f=fopen(path,"r+b");assert(f);u8 sector[512];
    assert(!fseek(f,512,SEEK_SET));assert(fread(sector,512,1,f)==1);
    for(unsigned c=149;c<=151;c++) put16(sector+c*2,0xFFFF);
    assert(!fseek(f,512,SEEK_SET));assert(fwrite(sector,512,1,f)==1);
    assert(!fseek(f,25*512,SEEK_SET));assert(fread(sector,512,1,f)==1);
    entry(sector+128,"LONGNA~1TXT",0x21,149,4);
    entry(sector+224,"VOLUME     ",0x08,0,0);
    entry(sector+256,"LIVE    BIN",0x20,150,3);
    assert(!fseek(f,25*512,SEEK_SET));assert(fwrite(sector,512,1,f)==1);
    memset(sector,0,512);entry(sector,".          ",0x10,2,0);
    entry(sector+32,"..         ",0x10,0,0);entry(sector+64,"KEEP    BIN",0x20,151,4);
    assert(!fseek(f,29*512,SEEK_SET));assert(fwrite(sector,512,1,f)==1);
    for(unsigned c=149;c<=151;c++) {
        memset(sector,0,512);memcpy(sector,c==150?"old":"keep",c==150?3:4);
        assert(!fseek(f,(long)(29+c-2)*512,SEEK_SET));assert(fwrite(sector,512,1,f)==1);
    }
    fclose(f);
}
static void write_protection_tests(const char *path) {
    write_fixture(path);m4_init(&board,"");m4_set_image(&board,path);assert(board.image_mounted);
    size_t size;u8 *before=image_bytes(path,&size);
    const char *denied[]={"/EIGHTCHR","/EIGHTCHR/","/","BIG.BIN","big.bin",
                          "Long named file.txt","longna~1.txt","/VOLUME"};
    const u8 modes[]={0x8A,0x92,0x0A,0x12}; /* replace/open-always, dynamic/fixed */
    for(unsigned n=0;n<sizeof(denied)/sizeof(*denied);n++) {
        for(unsigned m=0;m<sizeof(modes);m++) {
            const u8 *p=open_file(modes[m],denied[n]);
            assert(p[0]==0xFF && p[1]==M4_ERR_DENIED);
            assert(board.last_error==M4_ERR_DENIED);no_files();unchanged_image(path,before,size);
        }
    }
    const u8 *p=open_file(0x81,"BIG.BIN");assert(!p[1]);u8 fd=p[0];close_file(fd);
    p=open_file(0x81,"EIGHTCHR/KEEP.BIN");assert(!p[1]);fd=p[0];close_file(fd);
    p=open_file(0x81,"ABSENT.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_NOFILE);
    p=open_file(0x8A,"MISSING/NEW.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_NOPATH);
    no_files();unchanged_image(path,before,size);
    /* The legacy FAT wrapper is guarded too, not just M4's command handler. */
    assert(!fat_open(&board.image_vol,"BIG.BIN",true));
    assert(!fat_open(&board.image_vol,"EIGHTCHR",true));
    FatOpenStatus result;
    assert(!fat_open_mode(&board.image_vol,"BIG.BIN",FAT_OPEN_ALWAYS,&result));
    assert(result==FAT_OPEN_DENIED);
    assert(!fat_open_mode(&board.image_vol,"ABSENT.BIN",FAT_OPEN_READ,&result));
    assert(result==FAT_OPEN_NOT_FOUND);
    assert(!fat_open_mode(&board.image_vol,"MISSING/NEW.BIN",FAT_OPEN_ALWAYS,&result));
    assert(result==FAT_OPEN_NO_PATH);
    FatFile *protected=fat_open(&board.image_vol,"Long named file.txt",false);assert(protected);
    protected->write_mode=true; /* legacy clients may attempt this promotion */
    assert(!fat_write(protected,"bad",3));fat_close(protected);
    const u8 malformed[]={0x8A,'L','I','V','E','.','B','I','N'};
    p=command(0x4301,malformed,sizeof(malformed));assert(p[0]==0xFF && p[1]==M4_ERR_BADNAME);
    p=open_file(0x8A,"");assert(p[0]==0xFF && p[1]==M4_ERR_BADNAME);
    /* Joining a long cwd must not silently open a truncated pathname. */
    char saved_cwd[M4_PATH_MAX];memcpy(saved_cwd,board.cwd,sizeof(saved_cwd));
    memset(board.cwd,'x',sizeof(board.cwd)-1);board.cwd[0]='/';board.cwd[sizeof(board.cwd)-1]=0;
    p=open_file(0x8A,"LIVE.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_BADNAME);
    memcpy(board.cwd,saved_cwd,sizeof(board.cwd));
    /* Device read-only state must reject even an otherwise writable file. */
    board.image_read_only=true;
    p=open_file(0x8A,"LIVE.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_RDONLY);
    p=open_file(0x92,"NEW.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_RDONLY);
    p=open_file(0x81,"LIVE.BIN");assert(!p[1]);fd=p[0];close_file(fd);
    board.image_read_only=false;
    /* An unreadable lookup is not a missing file: no create fallback. */
    FILE *actual=board.image_vol.fp,*empty=tmpfile();assert(empty);
    board.image_vol.fp=empty;
    p=open_file(0x92,"LIVE.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_IO);
    assert(!fseek(empty,0,SEEK_END) && ftell(empty)==0);
    board.image_vol.fp=actual;fclose(empty);
    no_files();
    unchanged_image(path,before,size);
    /* Exhaustion must be detected before destructive open/truncate. */
    u8 descriptors[M4_MAX_FDS-2];
    for(unsigned i=0;i<sizeof(descriptors);i++) {
        p=open_file(0x81,"LIVE.BIN");assert(!p[1]);descriptors[i]=p[0];
    }
    p=open_file(0x8A,"LIVE.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_FULL);
    p=open_file(0x92,"NEW.BIN");assert(p[0]==0xFF && p[1]==M4_ERR_FULL);
    unchanged_image(path,before,size);
    for(unsigned i=0;i<sizeof(descriptors);i++) close_file(descriptors[i]);
    no_files();unchanged_image(path,before,size);
    /* OPEN_ALWAYS is non-destructive and leaves append positioning to SEEK. */
    p=open_file(0x92,"LIVE.BIN");assert(!p[1]);fd=p[0];
    p=command(0x4311,&fd,1);assert(get32(p)==3);
    close_file(fd);unchanged_image(path,before,size);free(before);
    p=open_file(0x92,"LIVE.BIN");assert(!p[1]);fd=p[0];
    u8 seek[]={fd,3,0,0,0};assert(!command(0x4305,seek,sizeof(seek))[0]);
    u8 data[]={fd,'t','a','i','l'};assert(!command(0x4303,data,sizeof(data))[0]);
    close_file(fd);file_contents("LIVE.BIN","oldtail",7);
    p=open_file(0x8A,"LIVE.BIN");assert(!p[1]);fd=p[0];
    data[0]=fd;assert(!command(0x4303,data,sizeof(data))[0]);close_file(fd);
    file_contents("LIVE.BIN","tail",4);
    p=open_file(0x8A,"LIVE.BIN");assert(!p[1]);close_file(p[0]);file_contents("LIVE.BIN","",0);
    p=open_file(0x92,"NEW.BIN");assert(!p[1]);fd=p[0];
    data[0]=fd;assert(!command(0x4303,data,sizeof(data))[0]);close_file(fd);
    file_contents("NEW.BIN","tail",4);
    p=open_file(0x0A,"FIXED.BIN");assert(!p[1] && p[0]==2);close_file(2);
    file_contents("FIXED.BIN","",0);
    file_contents("EIGHTCHR/KEEP.BIN","keep",4);
    file_contents("Long named file.txt","keep",4);
    no_files();m4_set_image(&board,"");m4_reset(&board);
}
static void host_tests(const char *root) {
    char file[512], dir[512], longfile[512];
    snprintf(file,sizeof(file),"%s/BIG.BIN",root);
    snprintf(dir,sizeof(dir),"%s/EIGHTCHR",root);
    snprintf(longfile,sizeof(longfile),"%s/Long named file.txt",root);
    assert(!mkdir(dir,0700));
    FILE *f=fopen(file,"wb"); assert(f); assert(!ftruncate(fileno(f),0x12345)); fclose(f);
    f=fopen(longfile,"wb"); assert(f); fclose(f);
    struct utimbuf stamp={1700000000,1700000000}; assert(!utime(file,&stamp));
    assert(!chmod(file,0400));
    m4_init(&board,root);
    const u8 *p=path_command(0x4316,"big.bin");
    assert(!p[0] && get32(p+1)==0x12345 && p[9]==0x21);
    assert((p[5]|p[6]) && !strcmp((const char *)p+10,"BIG.BIN"));
    p=path_command(0x4316,"EIGHTCHR"); assert(!p[0] && p[9]==0x10);
    p=path_command(0x4316,"Long named file.txt");
    assert(!p[0] && p[10]==0 && !strcmp((const char *)p+23,"Long named file.txt"));
    p=path_command(0x4316,"absent"); assert(p[0]==M4_ERR_NOFILE);
    path_command(0x4325,""); u8 limit=64; bool found=false;
    for (int i=0;i<4;i++) {
        p=command(0x4306,&limit,1);
        if (board.bus_mem[0]==2) break;
        if (!strcmp((const char *)p,">EIGHTCHR")) found=true;
    }
    assert(found); m4_reset(&board);
    assert(!unlink(file)); assert(!unlink(longfile)); assert(!rmdir(dir));
}
int main(void) {
    char root[]="/tmp/1984-m4-fs-XXXXXX"; assert(mkdtemp(root));
    char path[512]; snprintf(path,sizeof(path),"%s/card.img",root);
    image_fixture(path); image_tests(path); assert(!unlink(path));
    write_protection_tests(path);assert(!unlink(path));
    host_tests(root); assert(!rmdir(root));
    puts("M4 directory/FSTAT and protected write-open: image and host regressions passed");
    return 0;
}
