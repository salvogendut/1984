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
    host_tests(root); assert(!rmdir(root));
    puts("M4 directory/FSTAT: image and host regressions passed");
    return 0;
}
