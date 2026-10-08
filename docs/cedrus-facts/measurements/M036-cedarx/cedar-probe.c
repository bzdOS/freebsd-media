/* SPDX-License-Identifier: BSD-2-Clause */
/* Research host for an unmodified vendor binary. No codec algorithm here.
 * Generic buffer/API adapters are independent implementations. MMIO faults
 * forward to the existing bounded lab ioctl; all DMA storage has guards.
 * Vendor headers are separately downloaded public interface definitions.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <elf.h>
#include <dlfcn.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "videoengine.h"
#include "cedrus_lab.h"

static int labfd, tracefd;
static struct cedrus_lab_info info;
static unsigned char *arena, *registers;
static size_t cursor;
static unsigned mmio_count, frames;
static uint32_t sram_cursor;
static unsigned guard_count;
static size_t guard_offsets[256];
static uint64_t stack_guard = 0x2b78fa961d630400ULL;
static unsigned gVeVersion;

#include "elf-loader.h"

static uint32_t readreg(unsigned off)
{
    struct cedrus_lab_reg reg = {.off = off};
    if (ioctl(labfd, CEDRUS_LAB_RD, &reg))
        err(2, "lab read %#x", off);
    return reg.val;
}

static int dma_valid(uint32_t relative)
{
    uint64_t pa = (uint64_t)relative + info.dma_base;
    return pa >= info.buf_pa && pa < info.buf_pa + info.buf_size;
}

static void writereg(unsigned off, uint32_t value)
{
    /* Prevent the native driver's ISR consuming the vendor's lab completion. */
    if (off == 0x220)
        value &= ~7U;
    if (off == 0x230) {
        uint32_t address = (value & 0x0ffffff0U) | ((value & 15U) << 28);
        if (!dma_valid(address))
            errx(2, "VLD address outside lab: %#x", value);
    }
    if ((off == 0x23c || off == 0x250 || off == 0x254 || off == 0x244 ||
         off == 0x248) && value && !dma_valid(value))
        errx(2, "DMA address outside lab: reg %#x value %#x", off, value);
    if (off == 0x2e0)
        sram_cursor = value;
    if (off == 0x2e4) {
        if (sram_cursor >= 0x400 && sram_cursor < 0x620) {
            unsigned word = ((sram_cursor - 0x400) / 4) % 8;
            if (word >= 3 && word <= 6 && value && !dma_valid(value))
                errx(2, "DPB address outside lab: SRAM %#x value %#x",
                     sram_cursor, value);
        }
        sram_cursor += 4;
    }
    struct cedrus_lab_reg reg = {.off = off, .val = value};
    if (ioctl(labfd, CEDRUS_LAB_WR, &reg))
        err(2, "lab write %#x", off);
}

static __register_t *context_reg(ucontext_t *uc, unsigned index)
{
    if (index < 30) return &uc->uc_mcontext.mc_gpregs.gp_x[index];
    if (index == 30) return &uc->uc_mcontext.mc_gpregs.gp_lr;
    return NULL;
}

static void mmio_fault(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = context;
    uintptr_t address = (uintptr_t)si->si_addr;
    uintptr_t pc = uc->uc_mcontext.mc_gpregs.gp_elr;
    if (address < (uintptr_t)registers || address >= (uintptr_t)registers + 4096) {
        dprintf(2, "NON-MMIO fault signal=%d address=%#lx pc=%#lx plugin=%#lx\n",
                sig, address, pc, (uintptr_t)plugin.base);
        _exit(3);
    }
    uint32_t instruction = *(uint32_t *)pc;
    unsigned size = instruction >> 30, op = (instruction >> 22) & 3;
    unsigned rt = instruction & 31, rn = (instruction >> 5) & 31;
    int immediate = (instruction & 0x3b000000U) == 0x39000000U;
    int unscaled = (instruction & 0x3b200000U) == 0x38000000U;
    int offset = (instruction & 0x3b200c00U) == 0x38200800U;
    if ((!immediate && !unscaled && !offset) || size != 2 || op > 1 ||
        (address & 3) || ++mmio_count > 200000) {
        dprintf(2, "UNHANDLED MMIO instruction=%#x off=%#lx count=%u\n",
                instruction, address - (uintptr_t)registers, mmio_count);
        _exit(3);
    }
    unsigned off = address - (uintptr_t)registers;
    __register_t *target = context_reg(uc, rt);
    uint32_t value = 0;
    if (op == 1) {
        value = readreg(off);
        if (target) *target = value;
    } else {
        value = target ? *target : 0;
        writereg(off, value);
    }
    dprintf(tracefd, "%u %c %03x %08x\n", mmio_count, op ? 'R' : 'W', off, value);
    if (unscaled) {
        unsigned mode = (instruction >> 10) & 3;
        if (mode == 1 || mode == 3) {
            int imm = (int32_t)(instruction << 11) >> 23;
            __register_t *base = context_reg(uc, rn);
            if (base) *base += imm;
            else uc->uc_mcontext.mc_gpregs.gp_sp += imm;
        }
    }
    uc->uc_mcontext.mc_gpregs.gp_elr += 4;
}

static void *dma_alloc(int bytes)
{
    if (bytes <= 0 || guard_count >= 256) return NULL;
    size_t size = (bytes + 4095UL) & ~4095UL;
    if (cursor + size + 4096 > info.buf_size)
        errx(2, "lab DMA arena exhausted: request %d, used %zu", bytes, cursor);
    void *result = arena + cursor;
    memset(result, 0, size);
    cursor += size;
    guard_offsets[guard_count++] = cursor;
    memset(arena + cursor, 0xa5, 4096);
    cursor += 4096;
    printf("ALLOC %d offset=%#zx guard=%#zx\n", bytes,
           (size_t)((unsigned char *)result - arena), cursor - 4096);
    return result;
}

static void dma_free(void *p) {(void)p;}
static void dma_sync(void *p, int n) {(void)p; (void)n; __asm volatile("dsb sy" ::: "memory");}
static void *ve_phy(void *p) {
    if ((unsigned char *)p < arena || (unsigned char *)p >= arena + info.buf_size)
        errx(2, "non-lab DMA pointer %p", p);
    return (void *)(uintptr_t)(info.buf_pa - info.dma_base + (unsigned char *)p - arena);
}
static void *ve_vir(void *p) {return arena + ((uintptr_t)p - (info.buf_pa - info.dma_base));}
static void *cpu_phy(void *p) {return (void *)((uintptr_t)ve_phy(p) + info.dma_base);}
static void *cpu_vir(void *p) {return ve_vir((void *)((uintptr_t)p - info.dma_base));}
static int mem_open(void) {return 0;}
static void mem_close(void) {}
static int mem_size(void) {return info.buf_size;}
static int mem_set(void *p,int c,size_t n) {memset(p,c,n);return 0;}
static int mem_copy(void *d,void *s,size_t n) {memcpy(d,s,n);return 0;}
static struct ScMemOpsS memops = {
    .open=mem_open,.close=mem_close,.total_size=mem_size,.palloc=dma_alloc,
    .pfree=dma_free,.flush_cache=dma_sync,.ve_get_phyaddr=ve_phy,.ve_get_viraddr=ve_vir,
    .cpu_get_phyaddr=cpu_phy,.cpu_get_viraddr=cpu_vir,.mem_set=mem_set,
    .mem_cpy=mem_copy,.mem_read=mem_copy,.mem_write=mem_copy,.setup=mem_open,
    .shutdown=mem_open,.palloc_secure=dma_alloc
};

static void *AdapterVeGetBaseAddress(void) {return registers;}
static int AdapterVeWaitInterrupt(void) {
    for (unsigned i=0;i<2000;i++) {
        if (readreg(0x228)&7) return 1;
        usleep(1000);
    }
    fprintf(stderr,"WAIT timeout status=%#x error=%#x\n",readreg(0x228),readreg(0x2b8));
    return 0;
}
void ResetVeInternal(VideoEngine *p) {
    (void)p;
    if (ioctl(labfd,CEDRUS_LAB_ENGINE_RESET)) err(2,"reset");
    writereg(0,0x130001);
}

int GetBufferSize(int fmt,int w,int h,int *y,int *c,int *ys,int *cs,int align) {
    if (fmt != PIXEL_FORMAT_NV12 && fmt != PIXEL_FORMAT_NV21 &&
        fmt != PIXEL_FORMAT_YUV_MB32_420) errx(2,"unsupported buffer format %d",fmt);
    if (align<16) align=16;
    if (fmt == PIXEL_FORMAT_YUV_MB32_420 && align < 32) align=32;
    int stride=(w+align-1)/align*align;
    int luma=stride*((h+31)&~31);
    int chroma=fmt==PIXEL_FORMAT_YUV_MB32_420 ? stride*((h/2+31)&~31) : luma/2;
    if(ys)*ys=stride;
    if(cs)*cs=stride;
    if(y)*y=luma;
    if(c)*c=chroma;
    return 0;
}

static void save_picture(VideoPicture *p) {
    char file[100]; snprintf(file,sizeof(file),"picture-%u.bin",frames);
    int y,c,ys,cs;
    GetBufferSize(p->ePixelFormat,p->nWidth,p->nHeight,&y,&c,&ys,&cs,p->nLineStride);
    FILE *f=fopen(file,"wb");if(!f)err(2,"picture");
    fwrite(p->pData0,1,y,f);fwrite(p->pData1,1,c,f);fclose(f);
    printf("PICTURE %u id=%d pts=%jd format=%d size=%dx%d stride=%d bytes=%d+%d offsets=%d,%d,%d,%d errors=%d,%d\n",
        frames++,p->nID,(intmax_t)p->nPts,p->ePixelFormat,p->nWidth,p->nHeight,
        p->nLineStride,y,c,p->nTopOffset,p->nLeftOffset,p->nBottomOffset,p->nRightOffset,
        p->bTopFieldError,p->bBottomFieldError);
}

Fbm *FbmCreate(FbmCreateInfo *cfg,VideoFbmInfo *fi) {
    printf("FBM frames=%d size=%dx%d format=%d align=%d software=%d\n",
        cfg->nFrameNum,cfg->nWidth,cfg->nHeight,cfg->ePixelFormat,cfg->nAlignStride,cfg->bIsSoftDecoderFlag);
    if (cfg->nFrameNum<1||cfg->nFrameNum>32||cfg->bIsSoftDecoderFlag) errx(2,"invalid hardware FBM");
    Fbm *f=calloc(1,sizeof(*f));f->nMaxFrameNum=cfg->nFrameNum;f->nEmptyBufferNum=cfg->nFrameNum;
    f->nAlignValue=cfg->nAlignStride<16?16:cfg->nAlignStride;
    f->pFrames=calloc(cfg->nFrameNum,sizeof(*f->pFrames));
    for(int i=0;i<cfg->nFrameNum;i++) {
        VideoPicture *p=&f->pFrames[i].vpicture;
        int y,c,ys,cs; GetBufferSize(cfg->ePixelFormat,cfg->nWidth,cfg->nHeight,&y,&c,&ys,&cs,f->nAlignValue);
        p->nID=i;p->ePixelFormat=cfg->ePixelFormat;p->nWidth=cfg->nWidth;p->nHeight=cfg->nHeight;p->nLineStride=ys;
        p->pData0=dma_alloc(y+c);p->pData1=p->pData0+y;
        p->phyYBufAddr=(uintptr_t)ve_phy(p->pData0);p->phyCBufAddr=(uintptr_t)ve_phy(p->pData1);
    }
    if(fi) {fi->pFbmFirst=f;fi->nValidBufNum=cfg->nFrameNum;}
    return f;
}
void FbmDestroy(Fbm *f) {if(f){free(f->pFrames);free(f);}}
int FbmEmptyBufferNum(Fbm *f) {return f->nEmptyBufferNum;}
int FbmValidPictureNum(Fbm *f) {(void)f;return 0;}
int FbmGetAlignValue(Fbm *f) {return f->nAlignValue;}
unsigned FbmGetBufferOffset(Fbm *f,int y) {(void)f;(void)y;return 0;}
VideoPicture *FbmRequestBuffer(Fbm *f) {
    for(int i=0;i<f->nMaxFrameNum;i++) if(!f->pFrames[i].Flag.bUsedByDecoder) {
        f->pFrames[i].Flag.bUsedByDecoder=1;f->nEmptyBufferNum--;
        return &f->pFrames[i].vpicture;
    }
    return NULL;
}
void FbmReturnBuffer(Fbm *f,VideoPicture *p,int valid) {
    for(int i=0;i<f->nMaxFrameNum;i++) if(p==&f->pFrames[i].vpicture) {
        if(valid)save_picture(p);
        if(f->pFrames[i].Flag.bUsedByDecoder){f->pFrames[i].Flag.bUsedByDecoder=0;f->nEmptyBufferNum++;}
        return;
    }
    errx(2,"foreign FBM picture");
}
void FbmShareBuffer(Fbm *f,VideoPicture *p) {(void)f;save_picture(p);}

static VideoStreamDataInfo packet;
static int requested, flushed;
void *SbmBufferAddress(Sbm *s) {return s->pStreamBuffer;}
int SbmBufferSize(Sbm *s) {return s->nStreamBufferSize;}
char *SbmBufferWritePointer(Sbm *s) {return s->pWriteAddr;}
VideoStreamDataInfo *SbmRequestStream(Sbm *s) {
    (void)s;if(requested||flushed)return NULL;requested=1;return &packet;
}
int SbmReturnStream(Sbm *s,VideoStreamDataInfo *p) {(void)s;(void)p;requested=0;return 0;}
int SbmFlushStream(Sbm *s,VideoStreamDataInfo *p) {(void)s;(void)p;flushed=1;return 0;}
int VDecoderRegister(enum EVIDEOCODECFORMAT fmt,char *desc,VDecoderCreator *p,int soft) {
    (void)p;printf("REGISTER format=%d description=%s soft=%d\n",fmt,desc,soft);return 0;
}
static int android_log(int priority,const char *tag,const char *fmt,...) {
    (void)priority;va_list ap;va_start(ap,fmt);fprintf(stderr,"VENDOR %s: ",tag);
    int n=vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);return n;
}
static int foreign_atfork(void *a,void *b,void *c,void *d) {(void)a;(void)b;(void)c;(void)d;return 0;}
static int foreign_atexit(void *a,void *b,void *c) {(void)a;(void)b;(void)c;return 0;}
static void foreign_finalize(void *p) {(void)p;}

static void *foreign_resolve(const char *name) {
#define IMPORT(fn) if(strcmp(name,#fn)==0)return (void *)(uintptr_t)&fn
    IMPORT(AdapterVeGetBaseAddress);IMPORT(AdapterVeWaitInterrupt);IMPORT(ResetVeInternal);
    IMPORT(GetBufferSize);IMPORT(FbmCreate);IMPORT(FbmDestroy);IMPORT(FbmEmptyBufferNum);
    IMPORT(FbmReturnBuffer);IMPORT(FbmShareBuffer);IMPORT(FbmValidPictureNum);
    IMPORT(FbmGetAlignValue);IMPORT(FbmGetBufferOffset);IMPORT(FbmRequestBuffer);
    IMPORT(SbmBufferAddress);IMPORT(SbmBufferSize);IMPORT(SbmBufferWritePointer);
    IMPORT(SbmRequestStream);IMPORT(SbmReturnStream);IMPORT(SbmFlushStream);IMPORT(VDecoderRegister);
#undef IMPORT
    if(!strcmp(name,"gVeVersion"))return &gVeVersion;
    if(!strcmp(name,"__stack_chk_guard"))return &stack_guard;
    if(!strcmp(name,"__android_log_print"))return android_log;
    if(!strcmp(name,"__register_atfork"))return foreign_atfork;
    if(!strcmp(name,"__cxa_atexit"))return foreign_atexit;
    if(!strcmp(name,"__cxa_finalize"))return foreign_finalize;
    return dlsym(RTLD_DEFAULT,name);
}

int main(int argc,char **argv) {
    if(argc!=3)errx(2,"usage: cedar-probe libawh264.so stream.264");
    setvbuf(stdout,NULL,_IONBF,0);
    labfd=open("/dev/cedrus-lab",O_RDWR);if(labfd<0)err(2,"lab");
    if(ioctl(labfd,CEDRUS_LAB_INFO,&info))err(2,"lab info");
    arena=mmap(NULL,info.buf_size,PROT_READ|PROT_WRITE,MAP_SHARED,labfd,0);
    registers=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(arena==MAP_FAILED||registers==MAP_FAILED)err(2,"arena mmap");
    tracefd=open("mmio.txt",O_WRONLY|O_CREAT|O_TRUNC,0600);if(tracefd<0)err(2,"trace");
    struct sigaction sa={.sa_sigaction=mmio_fault,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);
    if(sigaction(SIGSEGV,&sa,NULL)||sigaction(SIGBUS,&sa,NULL))err(2,"signal handler");
    uint32_t version=readreg(0xf0);gVeVersion=version>0xffff?version>>16:version;
    printf("LAB physical=%#jx size=%ju version=%#x decoded=%#x\n",
        (uintmax_t)info.buf_pa,(uintmax_t)info.buf_size,version,gVeVersion);
    foreign_load(argv[1]);
    VDecoderCreator *create=foreign_symbol("CreateH264Decoder");if(!create)errx(2,"factory absent");
    VideoEngine engine={0};VConfig *cfg=&engine.vconfig;
    cfg->memops=&memops;cfg->eOutputPixelFormat=PIXEL_FORMAT_NV12;cfg->nAlignStride=16;
    cfg->nVbvBufferSize=1024*1024;cfg->nDecodeSmoothFrameBufferNum=2;
    engine.videoStreamInfo.eCodecFormat=VIDEO_CODEC_FORMAT_H264;
    ResetVeInternal(&engine);
    DecoderInterface *dec=create(&engine);if(!dec)errx(2,"factory failed");
    printf("FACTORY created\n");
    int result=dec->Init(dec,cfg,&engine.videoStreamInfo,&engine.fbmInfo);
    printf("INIT result=%d\n",result);if(result) return 4;
    FILE *src=fopen(argv[2],"rb");if(!src)err(2,"stream");
    fseek(src,0,SEEK_END);long bytes=ftell(src);rewind(src);if(bytes<=0||bytes>512*1024)errx(2,"stream size");
    Sbm sbm={0};sbm.pStreamBuffer=dma_alloc(cfg->nVbvBufferSize);
    sbm.nStreamBufferSize=cfg->nVbvBufferSize;sbm.pStreamBufferEnd=sbm.pStreamBuffer+sbm.nStreamBufferSize-1;
    if(fread(sbm.pStreamBuffer,1,bytes,src)!=(size_t)bytes)err(2,"stream read");fclose(src);
    sbm.pWriteAddr=sbm.pStreamBuffer+bytes;sbm.memops=&memops;sbm.nValidDataSize=bytes;
    packet.pData=sbm.pStreamBuffer;packet.nLength=bytes;packet.nPts=0;packet.nPcr=0;
    packet.bIsFirstPart=1;packet.bIsLastPart=1;packet.bValid=1;
    result=dec->SetSbm(dec,&sbm,0);printf("SET_SBM result=%d\n",result);if(result)return 4;
    for(unsigned i=0;i<32;i++) {
        result=dec->Decode(dec,1,0,0,0);
        printf("DECODE call=%u result=%d frames=%u status=%#x error=%#x\n",
            i,result,frames,readreg(0x228),readreg(0x2b8));
        if(result<0||result==VDECODE_RESULT_NO_BITSTREAM)break;
    }
    dec->Destroy(dec);
    unsigned intact=0;
    for(unsigned i=0;i<guard_count;i++) {
        int good=1;for(unsigned j=0;j<4096;j++)if(arena[guard_offsets[i]+j]!=0xa5)good=0;
        printf("GUARD %u offset=%#zx intact=%d\n",i,guard_offsets[i],good);intact+=good;
    }
    printf("RESULT frames=%u mmio=%u guards=%u/%u used=%zu\n",frames,mmio_count,intact,guard_count,cursor);
    return intact==guard_count&&frames?0:5;
}
