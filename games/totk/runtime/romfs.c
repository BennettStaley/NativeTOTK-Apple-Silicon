/* Read-only game RomFS, backed by the player's fully IVFC-verified image. */
#include "runtime.h"
#include "game_data.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include "mods.h"
#include "world_survey.h"
#include "../third_party/zstd/zstd.h"
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
/* The image's size and SHA-256 and the index's SHA-256 from the player's verified extraction: romfs-config.txt in the game
   data folder (game_data.h, scripts/prepare_romfs_backend.py), read when the RomFS is opened. */
static uint64_t romfs_image_size;static const char *romfs_image_sha256,*romfs_index_sha256;
#define ROMFS_IMAGE_SIZE romfs_image_size
#define ROMFS_IMAGE_SHA256 romfs_image_sha256
#define ROMFS_INDEX_SHA256 romfs_index_sha256
static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count,strings_offset;
static int image_fd=-1;
/* Split image: romfs.bin.000, .001, ... of equal size (the last one shorter), read in place. The phone gets the 17 GB
   image this way: devicectl drops the connection on a single file of that size, and joining the parts on the device
   would need the space twice. image_fd is then the first part; src_pread routes image offsets to their part. */
#define MAX_CHUNKS 64
static int chunk_fds[MAX_CHUNKS];
static unsigned nchunks;
static uint64_t chunk_size;
static ssize_t src_pread(int fd,void *buf,size_t n,off_t off) {
 if(!nchunks||fd!=image_fd)return pread(fd,buf,n,off);
 uint64_t c=(uint64_t)off/chunk_size,o=(uint64_t)off%chunk_size;if(c>=nchunks)return 0;
 if(n>chunk_size-o)n=(size_t)(chunk_size-o);
 return pread(chunk_fds[c],buf,n,(off_t)o);
}
static const uint8_t *image_map;
static void need(int c,const char *m) {if(!c)runtime_fail(m);}
/* Block cache for small reads of the image. Entering the world the game reads ~3,500 small files (median 2 KB) on two
   loader threads, mostly next to each other in the image, and each read was a cold pread of 80-390 us (a fork's 1.0.0
   trace, DavidZande/MacNativeGames 2026-10-20). Reads under BLK bytes are served from BLK-aligned blocks of the image
   (NBLK of them, least recently used out), so neighbouring files share one disk read. The image is read-only, so a block
   never goes stale. Bigger reads and overlays go to the file as before. TOTK_ROMFS_CACHE=0 turns it off.
   Mods: only reads of handles whose source is the image itself (fd == image_fd, handle_source) come here; a file a mod or
   the build's patches replace or add is opened on its overlay's own fd, so the mod's copy always wins. The overlay table
   is fixed at mount (mods load at start), and the cache holds image bytes only (keyed by image offset), so a mod change
   between runs can never be served stale bytes. */
#define BLK (256u<<10)
#if TARGET_OS_IPHONE
#define NBLK 64   /* 16 MB: the phone's memory is tighter */
#else
#define NBLK 256   /* 64 MB */
#endif
static struct {uint64_t at,used;uint8_t *bytes;} blocks[NBLK];   /* at: image offset + 1 (0 empty); used: last use */
static uint64_t block_clock;
static pthread_mutex_t block_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t block_hits,block_misses;
static int block_cache_on(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_ROMFS_CACHE");v=!(e&&!strcmp(e,"0"));if(!v)fprintf(stderr,"[totk] TOTK_ROMFS_CACHE=0: small RomFS reads go to the file\n");}return v;}
/* Copies n bytes at image offset off (within one block) from the cache; fills the block from the image on a miss. */
static void block_read(void *dst,uint64_t off,size_t n){
 uint64_t at=off/BLK*BLK;size_t in=(size_t)(off-at);
 pthread_mutex_lock(&block_lock);
 for(unsigned i=0;i<NBLK;i++)if(blocks[i].at==at+1){blocks[i].used=++block_clock;memcpy(dst,blocks[i].bytes+in,n);pthread_mutex_unlock(&block_lock);atomic_fetch_add(&block_hits,1);return;}
 pthread_mutex_unlock(&block_lock);
 uint8_t *b=malloc(BLK);need(b!=NULL,"RomFS block cache");size_t got=0;
 while(got<BLK&&at+got<ROMFS_IMAGE_SIZE){ssize_t r=src_pread(image_fd,b+got,BLK-got,(off_t)(at+got));if(r<=0)runtime_fail("RomFS read failure");got+=(size_t)r;}
 need(in+n<=got,"RomFS block read past the image");
 memcpy(dst,b+in,n);atomic_fetch_add(&block_misses,1);
 pthread_mutex_lock(&block_lock);   /* the other loader thread may have read the same block meanwhile: then keep one */
 unsigned slot=0;int dup=0;
 for(unsigned i=0;i<NBLK;i++){if(blocks[i].at==at+1){dup=1;break;}if(blocks[i].used<blocks[slot].used)slot=i;}
 if(!dup){free(blocks[slot].bytes);blocks[slot].bytes=b;blocks[slot].at=at+1;blocks[slot].used=++block_clock;b=NULL;}
 pthread_mutex_unlock(&block_lock);free(b);
 uint64_t m=atomic_load(&block_misses);if(m==1||m%4096==0)fprintf(stderr,"[totk] RomFS block cache: %llu block reads for %llu small reads\n",(unsigned long long)m,(unsigned long long)(m+atomic_load(&block_hits)));
}
static char mount_name[32];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t read_calls,read_bytes,read_ns,read_prepare_ns,read_report_ns;
static uint64_t handles[NH]; /* file index + 1; 0 is free */
/* Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
   path, the overlay's size and SHA-256 and the original's SHA-256; a missing path, a size or hash mismatch or a changed
   original stops the start. The directories, in this order: the build's patches (scripts/make_romfs_patches.py;
   TOTK_ROMFS_PATCHES, default extracted/<version>/romfs-patches, 0 = none), the true-2K HeapDef overlay
   (scripts/make_output_res_overlay.py) and the player's mods (TOTK_ROMFS_MODS, which the launcher sets to
   <data>/mods/<version>; no manifest there = none). Each file can be replaced once, except that a mod file merged with the
   build's edit takes the place of the build's overlay (overlay_load). An overlay file is opened when the
   game opens it, so a mod can replace thousands of files (a dub's voice streams) without holding a descriptor each; the
   file's identity (device, inode, size, modification time) is kept from the start check, and an open that finds another
   file there stops the game instead of serving bytes nobody checked. scripts/make_mod_manifest.py writes a mod's manifest.
   A line whose original SHA-256 is "-" adds a file the RomFS lacks (issue #48: The Second Quest adds 874): it is numbered
   after the RomFS's own files (file_count + k, k in path order, added[]), opens and reads like a replacement, and its
   folders exist for runtime_romfs_entry_type. The RomFS has no directory listing here, so nothing else needs to know.
   The player's layers (that older folder, then the mods folder of mods.h: TOTK_MODS_DIR, one folder per mod, mods.txt for
   the order, no manifest) never stop the start over a file a build layer also changes (build_yield, mods_build_collision):
   the player's copy is used and the build's patch of that file is dropped, except for what the port needs (the HeapDef
   overlay: the heap sizes for the output), where the build's copy is kept; the Common layout archive that UI mods ship
   gets the build's quick-menu background fix applied again (capture_refix). The log says which. The mods folder's files
   are not hashed; their identity check at open time is the same. When player files are loaded, the ResourceSizeTable is
   served adjusted for them (rstb_serve). A TOTK_ROMFS_MODS line may also hold the build's edit already (its original's
   SHA-256 is that of the build's overlay, make_mod_manifest.py after upstream #134): it replaces that overlay in place. */
typedef struct { uint64_t index,size; char *path; dev_t dev; ino_t ino; struct timespec mtime; char *name; char sha[65]; } Overlay;
static Overlay *overlays; /* sorted by RomFS index after overlay_init */
static size_t noverlay,overlay_cap;
#define ADDED UINT64_MAX   /* an added file's index until overlay_init numbers it */
static char **added;       /* RomFS paths of the added files, sorted; added[k] is index file_count + k */
static size_t nadded;
static uint32_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
static int handle_fd[NH];       /* the handle's open overlay file, when handle_ovl is set */
typedef struct { uint64_t offset,size;uint32_t name,len; } Entry;
_Static_assert(sizeof(Entry)==24,"index entry layout");
static Entry entry(uint64_t i) {Entry e;memcpy(&e,index_bytes+24+i*24,24);return e;}
static const char *entry_name(Entry e) {return (char *)index_bytes+strings_offset+e.name;}
static void sha_fd(int fd,const char *expected,uint64_t total,const char *stage) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(off_t off=0;;){ssize_t n=src_pread(fd,buffer,8<<20,off);need(n>=0,"read hash input");if(!n)break;CC_SHA256_Update(&c,buffer,(CC_LONG)n);off+=n;
  if(stage&&((uint64_t)off%(256u<<20)<(8u<<20)||!n))runtime_boot_progress(stage,(uint64_t)off,total);}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);need(!strcmp(hex,expected),"RomFS integrity mismatch");
}
/* A successful full hash may be reused only while the same regular file has
   unchanged filesystem identity and modification metadata. This is a startup
   shortcut, not a fresh integrity proof; TOTK_ROMFS_VERIFY=full forces one. */
typedef struct {
 char magic[16], sha256[65];
 uint64_t dev,ino,size;
 int64_t mtime_sec,mtime_nsec,ctime_sec,ctime_nsec,birth_sec,birth_nsec;
} RomfsProof;
static RomfsProof proof_for(const struct stat *s) {
 RomfsProof p={0};memcpy(p.magic,"TOTKRFS-PROOF-1",16);memcpy(p.sha256,ROMFS_IMAGE_SHA256,65);
 p.dev=(uint64_t)s->st_dev;p.ino=(uint64_t)s->st_ino;p.size=(uint64_t)s->st_size;
 p.mtime_sec=s->st_mtimespec.tv_sec;p.mtime_nsec=s->st_mtimespec.tv_nsec;
 p.ctime_sec=s->st_ctimespec.tv_sec;p.ctime_nsec=s->st_ctimespec.tv_nsec;
 p.birth_sec=s->st_birthtimespec.tv_sec;p.birth_nsec=s->st_birthtimespec.tv_nsec;
 return p;
}
static int proof_equal(const RomfsProof *a,const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)
  &&a->size==b->size
#if TARGET_OS_IPHONE
  /* iOS rewrites ctime and birthtime when a sealed app bundle is read for the first time, so a fresh
     install never matched its own before/after stat ("FAIL: RomFS changed during verification").
     The record exists to identify the bytes, and the SHA256 pass above already proved them. */
  ;
#else
  &&a->dev==b->dev&&a->ino==b->ino
  &&a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec
  &&a->ctime_sec==b->ctime_sec&&a->ctime_nsec==b->ctime_nsec
  &&a->birth_sec==b->birth_sec&&a->birth_nsec==b->birth_nsec;
#endif
}
/* Split image: one record per part, all of which must be unchanged for the earlier full hash to be reused. */
static void chunk_proofs(RomfsProof *out) {
 /* ctime is left out: reinstalling the iOS app touches the data container's file metadata (ctime) without changing a
    byte, which made every launch hash 17 GB again. A content change still moves mtime (and size). */
 for(unsigned c=0;c<nchunks;c++){struct stat st;need(fstat(chunk_fds[c],&st)==0&&S_ISREG(st.st_mode),"RomFS part stat");out[c]=proof_for(&st);out[c].ctime_sec=out[c].ctime_nsec=0;}
}
static void verify_chunks(const char *dir) {
 RomfsProof want[MAX_CHUNKS],saved[MAX_CHUNKS],after[MAX_CHUNKS];size_t bytes=nchunks*sizeof(RomfsProof);chunk_proofs(want);
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;   /* iOS: directories devicectl creates belong to root */
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-parts-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&(size_t)ps.st_size==bytes&&read(proof_fd,saved,bytes)==(ssize_t)bytes;close(proof_fd);
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress("Game data verification record is valid",ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress("Verifying game data",0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,"Verifying game data");
 chunk_proofs(after);need(!memcmp(after,want,bytes),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-parts-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,want,bytes);int ok=n==(ssize_t)bytes&&fsync(out)==0;close(out);if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void open_chunks(const char *dir) {
 uint64_t total=0;
 for(;nchunks<MAX_CHUNKS;nchunks++){char path[4096];need(snprintf(path,sizeof(path),"%s/romfs.bin.%03u",dir,nchunks)<(int)sizeof(path),"RomFS part path");
  int fd=open(path,O_RDONLY);if(fd<0)break;struct stat st;need(fstat(fd,&st)==0&&st.st_size>0,"RomFS part stat");
  if(!nchunks)chunk_size=(uint64_t)st.st_size;else need(chunk_fds[nchunks-1]>=0&&total==(uint64_t)nchunks*chunk_size,"RomFS parts must have equal sizes except the last");
  chunk_fds[nchunks]=fd;total+=(uint64_t)st.st_size;}
 need(nchunks>0,"RomFS image missing (romfs.bin or romfs.bin.000...)");
 need(total==ROMFS_IMAGE_SIZE,"RomFS parts do not add up to the image size");
 image_fd=chunk_fds[0];
 fprintf(stderr,"[totk] RomFS image in %u parts of %llu bytes\n",nchunks,(unsigned long long)chunk_size);
}
static void verify_image(int fd,const char *dir) {
 if(nchunks){verify_chunks(dir);return;}
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;
 struct stat before,after;need(fstat(fd,&before)==0&&S_ISREG(before.st_mode),"RomFS image stat");
 RomfsProof want=proof_for(&before),saved;
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&ps.st_size==sizeof(saved)&&read(proof_fd,&saved,sizeof(saved))==sizeof(saved);close(proof_fd);
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] RomFS unchanged; reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);runtime_boot_progress("Game data verification record is valid",want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 (%llu bytes)%s\n",(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress("Verifying game data",0,want.size);sha_fd(fd,ROMFS_IMAGE_SHA256,want.size,"Verifying game data");
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after);need(proof_equal(&want,&finished),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,&want,sizeof(want));int ok=n==sizeof(want)&&fsync(out)==0;close(out);
  if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 size_t cap=size<(8u<<20)?(size_t)size:(8u<<20);CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(cap?cap:1);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>cap?cap:(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");CC_SHA256_Update(&c,buffer,(CC_LONG)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static size_t nbuild;   /* overlays[0..nbuild): the build's own layers (patches, HeapDef); the player's come after */
static size_t npatch;   /* overlays[0..npatch): scripts/make_romfs_patches.py; [npatch..nbuild): the HeapDef overlay */
static size_t build_owner(uint64_t i,const char *name);
static int build_yield(size_t owner,const char *who,const char *name);
static int capture_refix(const char *who,const char *name,const char *file,const struct stat *st,char *out,size_t cap);
/* With player set (TOTK_ROMFS_MODS), a line whose original SHA-256 is that of a build overlay loaded before it for the same
   path (the build's romfs-patches) replaces that overlay: scripts/make_mod_manifest.py wrote it by applying the build's edit
   to the mod's own file (upstream #134, issue #73: translations replace the UI archive the build patches for the quick-menu
   capture). Its original is then the build's checked file, so the RomFS original is not read again. Any other collision
   with a build layer goes to build_yield. */
static void overlay_load(const char *dir,int required,int player) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];size_t first=noverlay,replaced=0;uint64_t hashed=0,t0=runtime_now_ns();
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]",name,&size,sha,orig)==4&&strlen(sha)==64&&(strlen(orig)==64||!strcmp(orig,"-")),"RomFS overlay manifest line");
  need(path_ok(name),"RomFS overlay path");uint64_t i=lower(name);int known=i<file_count&&!strcmp(entry_name(entry(i)),name),add=!strcmp(orig,"-");
  if(!known&&!add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay names a file the RomFS lacks");}
  if(known&&add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay adds a file the RomFS already has");}
  /* The build's edit already in the player's file (its original is the build overlay's SHA-256): replaces that overlay. */
  size_t over=SIZE_MAX;if(player&&known)for(size_t k=0;k<nbuild&&k<first;k++)if(overlays[k].index==i&&!strcmp(overlays[k].sha,orig)){over=k;break;}
  /* Another player's file the build's layers also change: build_yield decides (the build's copy where the port needs it). */
  size_t owner=player&&over==SIZE_MAX?build_owner(known?i:ADDED,name):0;int yield=owner?build_yield(owner,dir,name):0;
  if(owner&&!yield)continue;
  if(owner)first--;   /* the build's slot is gone */
  Entry e={0};if(known){e=entry(i);if(over==SIZE_MAX)sha_range(image_fd,e.offset,e.size,orig,"original");}else i=ADDED;
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY|O_CLOEXEC);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);close(fd);
  hashed+=(over==SIZE_MAX?e.size:0)+size;
  char fixed[4096];if(yield==MODS_BUILD_REFIX&&capture_refix(dir,name,file,&st,fixed,sizeof fixed)){need(stat(fixed,&st)==0,"RomFS overlay fixed copy");snprintf(file,sizeof file,"%s",fixed);size=(unsigned long long)st.st_size;}
  if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
  char *copy=strdup(file),*rel=strdup(name);need(copy&&rel,"RomFS overlay path copy");Overlay o={i,size,copy,st.st_dev,st.st_ino,st.st_mtimespec,rel,{0}};memcpy(o.sha,sha,65);
  if(over!=SIZE_MAX){fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) replaces the build's overlay %s (the mod holds its edit)\n",name,size,overlays[over].path);
   free(overlays[over].path);free(overlays[over].name);overlays[over]=o;replaced++;continue;}
  overlays[noverlay++]=o;
  if(noverlay-first<=16)fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) %s\n",name,size,add?"is added":"replaces the original");
 }
 fclose(m);
 if(noverlay-first>16)fprintf(stderr,"[totk] RomFS overlay: %zu files from %s (%zu more not listed)\n",noverlay-first+replaced,dir,noverlay-first-16);
 if(noverlay>first||replaced)fprintf(stderr,"[totk] RomFS overlay: %s checked, %zu files, %.1f MB hashed in %.2f s\n",dir,noverlay-first+replaced,hashed/1048576.0,(runtime_now_ns()-t0)/1e9);
}
/* The build layer's overlay slot + 1 that already changes RomFS file i (or adds name, i = ADDED), else 0. */
static size_t build_owner(uint64_t i,const char *name) {
 for(size_t k=0;k<nbuild;k++)if(overlays[k].index==i&&(i!=ADDED||!strcmp(overlays[k].name,name)))return k+1;return 0;
}
/* A player's file at a path build slot owner-1 changes: MODS_BUILD_KEEP (0 here: the build's copy stays, the player's is
   left out) for what the port needs, else the build's slot is removed and the player's copy is used (MODS_BUILD_MOD_WINS,
   or MODS_BUILD_REFIX: with the build's fix applied to it again). */
static char yielded[4096];   /* the build's copy of the file build_yield last gave up (capture_refix merges its edits) */
static int build_yield(size_t owner,const char *who,const char *name) {
 size_t k=owner-1;int r=k>=npatch?MODS_BUILD_KEEP:mods_build_collision(name);
 if(r==MODS_BUILD_KEEP){fprintf(stderr,"[totk] mods: %s: %s is also changed by the port (%s), which needs its own copy (the heap sizes for the output resolution); the mod's copy is left out\n",who,name,overlays[k].path);return 0;}
 fprintf(stderr,"[totk] mods: %s: %s is also changed by the port's own patches (%s); the mod's copy is used%s\n",who,name,overlays[k].path,
  r==MODS_BUILD_REFIX?", with the port's own edits applied to it again":" and the port's patch is left out");
 snprintf(yielded,sizeof yielded,"%s",r==MODS_BUILD_REFIX?overlays[k].path:"");
 free(overlays[k].path);free(overlays[k].name);memmove(&overlays[k],&overlays[k+1],(noverlay-k-1)*sizeof(*overlays));noverlay--;nbuild--;if(k<npatch)npatch--;
 return r;
}
static int overlay_order(const void *a,const void *b) {uint64_t x=((const Overlay *)a)->index,y=((const Overlay *)b)->index;return (x>y)-(x<y);}
static int added_order(const void *a,const void *b) {const Overlay *x=*(Overlay *const *)a,*y=*(Overlay *const *)b;return strcmp(x->name,y->name);}
/* Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop the start. */
static void added_init(void) {
 Overlay **list=NULL;for(size_t k=0;k<noverlay;k++)if(overlays[k].index==ADDED){list=realloc(list,(nadded+1)*sizeof(*list));need(list!=NULL,"RomFS added files");list[nadded++]=&overlays[k];}
 if(!nadded)return;
 qsort(list,nadded,sizeof(*list),added_order);added=malloc(nadded*sizeof(*added));need(added!=NULL,"RomFS added files");
 for(size_t k=0;k<nadded;k++){
  if(k&&!strcmp(list[k]->name,list[k-1]->name)){fprintf(stderr,"[totk] RomFS overlay: %s and %s both add %s\n",list[k-1]->path,list[k]->path,list[k]->name);runtime_fail("RomFS overlay: two overlays add the same file (see the log; remove it from one of the manifests)");}
  list[k]->index=file_count+k;added[k]=list[k]->name;}
 free(list);fprintf(stderr,"[totk] RomFS overlay: %zu files added to the RomFS\n",nadded);
}
/* The index of RomFS path p (an added file: file_count + k), or UINT64_MAX. */
static uint64_t file_index(const char *p) {
 uint64_t i=lower(p);if(i<file_count&&!strcmp(p,entry_name(entry(i))))return i;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],p)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strcmp(added[lo],p)?file_count+lo:UINT64_MAX;
}
/* 1 when a RomFS file or an added one lies under folder (path ending in '/'). */
static int folder_exists(const char *folder) {
 size_t n=strlen(folder);uint64_t i=lower(folder);if(i<file_count&&!strncmp(entry_name(entry(i)),folder,n))return 1;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],folder)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strncmp(added[lo],folder,n);
}
/* The overlay slot + 1 that replaces RomFS file i, or 0. */
static uint32_t overlay_slot(uint64_t i) {size_t lo=0,hi=noverlay;while(lo<hi){size_t m=lo+(hi-lo)/2;if(overlays[m].index<i)lo=m+1;else hi=m;}return lo<noverlay&&overlays[lo].index==i?(uint32_t)(lo+1):0;}
/* The player's mods folder (mods.h, runtime/mods.c): each enabled mod's files in load order, the first mod's copy of a path
   only, as overlay entries; no manifest and no hashing (nothing to compare with: a mod is whatever the player put there).
   The identity check at open time still applies. A file a build layer already changes: build_yield. */
static uint32_t *folded;   /* RomFS indices in case-insensitive order, built on the first miss */
static int folded_order(const void *a,const void *b) {return strcasecmp(entry_name(entry(*(const uint32_t *)a)),entry_name(entry(*(const uint32_t *)b)));}
static int romfs_lookup(const char *p,char *real,size_t cap) {
 uint64_t i=lower(p);if(i<file_count&&!strcmp(entry_name(entry(i)),p))return 1;
 if(!folded){folded=malloc(file_count*sizeof(*folded));need(folded!=NULL,"RomFS case index");for(uint64_t k=0;k<file_count;k++)folded[k]=(uint32_t)k;qsort(folded,file_count,sizeof(*folded),folded_order);}
 size_t lo=0,hi=file_count;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcasecmp(entry_name(entry(folded[m])),p)<0)lo=m+1;else hi=m;}
 if(lo<file_count&&!strcasecmp(entry_name(entry(folded[lo])),p)){snprintf(real,cap,"%s",entry_name(entry(folded[lo])));return 2;}
 return 0;
}
static void push_overlay(uint64_t i,const char *file,const char *name,const struct stat *st) {
 if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
 char *copy=strdup(file),*rel=strdup(name);need(copy&&rel,"RomFS overlay path copy");overlays[noverlay++]=(Overlay){i,(uint64_t)st->st_size,copy,st->st_dev,st->st_ino,st->st_mtimespec,rel,{0}};
}
static void mods_load(void) {
 const char *dir=mods_dir();if(!strcmp(dir,"0")){fprintf(stderr,"[totk] mods: off (TOTK_MODS_DIR=0)\n");return;}
 uint64_t t0=runtime_now_ns();mods_set_romfs_lookup(romfs_lookup);int n=mods_scan(),on=0;
 for(int k=0;k<n;k++){ModInfo m;if(mods_get(k,&m))continue;on+=m.enabled;
  fprintf(stderr,"[totk] mods: %d. %s%s: %u files, %.1f MB%s%s\n",k+1,m.folder,m.enabled?"":" (off)",m.files,m.bytes/1048576.0,strcmp(m.name,m.folder)?" - ":"",strcmp(m.name,m.folder)?m.name:"");
  for(unsigned b=MOD_HAS_CODE;b<=MOD_EMPTY;b<<=1)if(m.flags&b){
   if(b==MOD_CONFLICT)fprintf(stderr,"[totk] mods:    %u of its files are also changed by a mod higher in the list; that mod's copies are used\n",m.conflicts);
   else fprintf(stderr,"[totk] mods:    %s\n",mods_notice(b));}}
 const ModFile *f;int nf=mods_files(&f);size_t first=noverlay;unsigned replaced=0,addn=0,kept=0,overrode=0;
 for(int k=0;k<nf;k++){
  ModInfo m;if(mods_get(f[k].mod,&m))continue;
  if(!path_ok(f[k].path)){fprintf(stderr,"[totk] mods: %s: %s is not a usable RomFS path; left out\n",m.folder,f[k].path);continue;}
  uint64_t i=f[k].replaces?lower(f[k].path):ADDED;if(i!=ADDED&&(i>=file_count||strcmp(entry_name(entry(i)),f[k].path)))i=ADDED;
  size_t owner=0;for(size_t j=0;j<first&&!owner;j++)if(overlays[j].index==i&&(i!=ADDED||!strcmp(overlays[j].name,f[k].path)))owner=j+1;
  if(owner>nbuild){fprintf(stderr,"[totk] mods: %s: %s is also changed by the older mods/" TOTK_GAME_DIR " folder (%s); that copy is used\n",m.folder,f[k].path,overlays[owner-1].path);continue;}
  struct stat st;if(stat(f[k].file,&st)||!S_ISREG(st.st_mode)){fprintf(stderr,"[totk] mods: %s: %s is gone; left out\n",m.folder,f[k].file);continue;}
  int yield=owner?build_yield(owner,m.folder,f[k].path):0;
  if(owner&&!yield){kept++;continue;}
  if(owner){first--;overrode++;}   /* the build's slot is gone */
  const char *file=f[k].file;char fixed[4096];
  if(yield==MODS_BUILD_REFIX&&capture_refix(m.folder,f[k].path,f[k].file,&st,fixed,sizeof fixed)){need(stat(fixed,&st)==0,"mods: fixed copy stat");file=fixed;}
  push_overlay(i,file,f[k].path,&st);if(i==ADDED)addn++;else replaced++;
  if(noverlay-first<=16)fprintf(stderr,"[totk] mods: %s: %s (%llu bytes) %s\n",m.folder,f[k].path,(unsigned long long)st.st_size,i==ADDED?"is added":"replaces the original");
 }
 if(noverlay-first>16)fprintf(stderr,"[totk] mods: %zu more files not listed\n",noverlay-first-16);
 fprintf(stderr,"[totk] mods: %s: %d installed, %d on; %u files replace the game's, %u are added",dir,n,on,replaced,addn);
 if(overrode)fprintf(stderr,", %u in place of the port's own patches",overrode);
 if(kept)fprintf(stderr,", %u left to the port's own copies",kept);
 fprintf(stderr," (%.2f s)\n",(runtime_now_ns()-t0)/1e9);
 mods_mark_loaded();
}

/* The ResourceSizeTable for the player's files (mods.h): the original table with each changed or added resource's entry
   raised to TkSharp's estimate, served as a zstd frame of raw blocks. A mod's own table is never served (they are made
   for one game version and one set of mods). The result is kept in <mods>/.cache with a key of the files' names, sizes
   and identities, so a start with the same mods reads nothing but that. */
static uint8_t *image_file(uint64_t i,size_t *n) {
 Entry e=entry(i);uint8_t *b=malloc(e.size?e.size:1);need(b!=NULL,"RomFS file buffer");
 for(uint64_t done=0;done<e.size;){ssize_t r=src_pread(image_fd,b+done,(size_t)(e.size-done),(off_t)(e.offset+done));need(r>0,"RomFS read");done+=(uint64_t)r;}
 *n=(size_t)e.size;return b;
}
static uint8_t *host_file(const char *path,size_t *n,size_t limit) {
 int fd=open(path,O_RDONLY|O_CLOEXEC);struct stat st;if(fd<0)return NULL;
 if(fstat(fd,&st)||(uint64_t)st.st_size>limit){close(fd);return NULL;}
 uint8_t *b=malloc(st.st_size?(size_t)st.st_size:1);size_t done=0;
 while(b&&done<(size_t)st.st_size){ssize_t r=pread(fd,b+done,(size_t)st.st_size-done,(off_t)done);if(r<=0)break;done+=(size_t)r;}
 close(fd);if(b&&done!=(size_t)st.st_size){free(b);b=NULL;}*n=done;return b;
}
static ZSTD_DDict *ddicts[8];static unsigned ndicts;static int dicts_tried;
static uint8_t *zs_decompress(const uint8_t *src,size_t n,size_t *out);
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
/* A SARC archive's members (little-endian, as the game's): calls fn for each; 0 if it is not one. */
typedef void (*SarcFn)(void *ctx,const char *name,const uint8_t *data,size_t n);
static int sarc_each(const uint8_t *d,size_t n,SarcFn fn,void *ctx) {
 if(n<0x20||memcmp(d,"SARC",4)||d[6]!=0xFF||d[7]!=0xFE||memcmp(d+0x14,"SFAT",4))return 0;
 uint32_t data=le32(d+0x0C);unsigned count=(unsigned)d[0x1A]|(unsigned)d[0x1B]<<8;size_t sfnt=0x20+(size_t)count*16;
 if(sfnt+8>n||memcmp(d+sfnt,"SFNT",4))return 0;
 for(unsigned k=0;k<count;k++){const uint8_t *node=d+0x20+(size_t)k*16;uint32_t attr=le32(node+4),start=le32(node+8),end=le32(node+12);
  const char *name="";if(attr>>24){size_t at=sfnt+8+(size_t)(attr&0xFFFF)*4;if(at>=n||!memchr(d+at,0,n-at))return 0;name=(const char *)d+at;}
  if(start>end||(uint64_t)data+end>n)return 0;fn(ctx,name,d+data+start,end-start);}
 return 1;
}
static void add_dict(void *ctx,const char *name,const uint8_t *data,size_t n) {(void)ctx;(void)name;if(ndicts<8&&(ddicts[ndicts]=ZSTD_createDDict(data,n)))ndicts++;}
/* The game's zstd dictionaries (Pack/ZsDic.pack.zs), for mod files compressed with one. */
static void load_dicts(void) {
 if(dicts_tried)return;dicts_tried=1;uint64_t i=lower("Pack/ZsDic.pack.zs");if(i>=file_count||strcmp(entry_name(entry(i)),"Pack/ZsDic.pack.zs"))return;
 size_t n,m;uint8_t *z=image_file(i,&n),*pack=zs_decompress(z,n,&m);free(z);if(pack)sarc_each(pack,m,add_dict,NULL);free(pack);
}
static uint8_t *zs_decompress(const uint8_t *src,size_t n,size_t *out) {
 unsigned id=ZSTD_getDictID_fromFrame(src,n);const ZSTD_DDict *dict=NULL;
 if(id){load_dicts();for(unsigned k=0;k<ndicts;k++)if(ZSTD_getDictID_fromDDict(ddicts[k])==id)dict=ddicts[k];if(!dict)return NULL;}
 unsigned long long want=ZSTD_getFrameContentSize(src,n);if(want==ZSTD_CONTENTSIZE_ERROR||(want!=ZSTD_CONTENTSIZE_UNKNOWN&&want>(1ull<<30)))return NULL;
 size_t cap=want==ZSTD_CONTENTSIZE_UNKNOWN?n*4+65536:(size_t)want+1;uint8_t *b=malloc(cap);ZSTD_DCtx *c=ZSTD_createDCtx();
 if(!b||!c||(dict&&ZSTD_isError(ZSTD_DCtx_refDDict(c,dict)))){free(b);ZSTD_freeDCtx(c);return NULL;}
 ZSTD_inBuffer in={src,n,0};ZSTD_outBuffer o={b,cap,0};
 for(;;){size_t r=ZSTD_decompressStream(c,&o,&in);if(ZSTD_isError(r))break;if(!r){ZSTD_freeDCtx(c);*out=o.pos;return b;}
  if(o.pos==o.size){if(cap>=(1u<<30))break;uint8_t *g=realloc(b,cap*2);if(!g)break;b=g;cap*=2;o.dst=b;o.size=cap;}
  else if(in.pos==in.size)break;}   /* the frame ends early */
 free(b);ZSTD_freeDCtx(c);return NULL;
}
/* A player's Common layout archive (UI mods: button glyphs, HUD) with the build's own edits applied again: every edit of the
   build's copy (yielded, from build_yield) that the mod leaves as the game ships it (mods_port_merge: the quick-menu
   background fix and the layouts for the output's shape, such as Native 16:10; in a layout or animation the mod rebuilt, by pane
   name), or, without the build's copy, the quick-menu
   fix alone (mods_capture_fix). Served as a zstd frame of raw blocks from <mods>/.cache: capture-fix-<path digest>.blarc.zs,
   its .key holding the player file's and the build copy's size, modification time and path and the verdict, so the next
   start with the same files reads nothing. 1 = out names the file to serve; 0 = the player's file is served unchanged (its
   archive differs from the game's where the fix goes, or it does not read), and the log says the fix is not applied. */
static int capture_refix(const char *who,const char *name,const char *file,const struct stat *st,char *out,size_t cap) {
 uint64_t t0=runtime_now_ns();char key[8800],cache[4096],keyfile[4400],saved[9400]={0},port[4096];struct stat ps;
 snprintf(port,sizeof port,"%s",yielded);*yielded=0;
 uint64_t vi=lower(name);if(vi>=file_count||strcmp(entry_name(entry(vi)),name)||stat(port,&ps))*port=0;
 snprintf(key,sizeof key,"totk-capture-fix-3\t%llu\t%lld.%09ld\t%s\t%llu\t%lld.%09ld\t%s",(unsigned long long)st->st_size,(long long)st->st_mtimespec.tv_sec,(long)st->st_mtimespec.tv_nsec,file,
  *port?(unsigned long long)ps.st_size:0ull,*port?(long long)ps.st_mtimespec.tv_sec:0ll,*port?(long)ps.st_mtimespec.tv_nsec:0l,port);
 uint8_t d[32];CC_SHA256(file,(CC_LONG)strlen(file),d);
 snprintf(cache,sizeof cache,"%s/.cache",mods_dir());
 if(mkdir(cache,0755)&&errno!=EEXIST){const char *t=getenv("TMPDIR");snprintf(cache,sizeof cache,"%s",t&&*t?t:"/tmp");}
 if(snprintf(out,cap,"%s/capture-fix-%02x%02x%02x%02x%02x%02x%02x%02x.blarc.zs",cache,d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7])>=(int)cap)return 0;
 snprintf(keyfile,sizeof keyfile,"%s.key",out);
 FILE *kf=fopen(keyfile,"r");if(kf){size_t r=fread(saved,1,sizeof saved-1,kf);saved[r]=0;fclose(kf);}
 size_t kl=strlen(key);struct stat cs;
 if(!strncmp(saved,key,kl)&&saved[kl]=='\n'){
  const char *verdict=saved+kl+1;
  if(!strncmp(verdict,"fixed\n",6)&&!stat(out,&cs)&&S_ISREG(cs.st_mode)&&cs.st_size>0){
   fprintf(stderr,"[totk] mods: %s: %s: the port's own edits are applied to the mod's copy (cached: %s)\n",who,name,out);return 1;}
  if(!strncmp(verdict,"unchanged\t",10)){fprintf(stderr,"[totk] mods: %s: %s: %s (cached)\n",who,name,verdict+10);return 0;}
 }
 size_t n=0,m=0;uint8_t *raw=host_file(file,&n,256u<<20),*plain=raw?zs_decompress(raw,n,&m):NULL;free(raw);
 char why[300]="the archive does not decompress",line[600],done[400]="";int moved=-1;
 if(plain&&*port){   /* all the build's edits the mod leaves alone */
  size_t pn=0,pm=0,on=0,om=0;uint8_t *pz=host_file(port,&pn,256u<<20),*pp=pz?zs_decompress(pz,pn,&pm):NULL,*oz=image_file(vi,&on),*op=zs_decompress(oz,on,&om);free(pz);free(oz);
  ModsMerge ms;moved=pp&&op?mods_port_merge(plain,m,op,om,pp,pm,&ms,why,sizeof why):-1;free(pp);free(op);
  if(moved>=0)snprintf(done,sizeof done,"%u values in %u members (%u of them by pane name in %u members the mod rebuilt, %u values with no place there; %u values the mod also changes keep the mod's; %u members the mod dropped or rebuilt past reading keep the mod's)",
   ms.words,ms.members-ms.skipped,ms.by_name,ms.rebuilt,ms.lost,ms.kept,ms.skipped);
  else fprintf(stderr,"[totk] mods: %s: %s: the port's edits do not merge (%s); trying the quick-menu background fix alone\n",who,name,why);
 }
 if(plain&&moved<0){moved=mods_capture_fix(plain,m,why,sizeof why);if(moved>0)snprintf(done,sizeof done,"the quick-menu background fix alone: %d visibility keys moved from frame 1 to 0",moved);}
 if(moved<=0){
  if(moved<0)snprintf(line,sizeof line,"the port's own edits were not applied (%s); the mod's file is used unchanged, so the quick menus' background can be black at 60 fps and the layout keeps 16:9",why);
  else snprintf(line,sizeof line,"the mod's copy already has the port's own edits; used unchanged");
  fprintf(stderr,"[totk] mods: %s: %s: %s\n",who,name,line);free(plain);
  kf=fopen(keyfile,"w");if(kf){fprintf(kf,"%s\nunchanged\t%s\n",key,line);fclose(kf);}
  return 0;}
 size_t fn=0;uint8_t *frame=mods_zstd_raw_frame(plain,m,&fn);free(plain);if(!frame)return 0;
 char tmp[4300];snprintf(tmp,sizeof tmp,"%s.%ld.tmp",out,(long)getpid());int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
 int ok=fd>=0&&write(fd,frame,fn)==(ssize_t)fn;if(fd>=0)ok&=!close(fd);free(frame);
 if(!ok||rename(tmp,out)){unlink(tmp);fprintf(stderr,"[totk] mods: %s: %s: cannot write %s (%s); the mod's file is used without the port's own edits\n",who,name,out,strerror(errno));return 0;}
 kf=fopen(keyfile,"w");if(kf){fprintf(kf,"%s\nfixed\n",key);fclose(kf);}
 fprintf(stderr,"[totk] mods: %s: %s: the port's own edits are applied to the mod's copy (%s; %zu bytes, zstd raw blocks, %s, %.2f s)\n",
  who,name,done,fn,out,(runtime_now_ns()-t0)/1e9);
 return 1;
}
typedef struct {char **paths;uint32_t *values;size_t n,cap;} Estimates;
static void est_add(Estimates *e,const char *path,uint32_t v) {
 if(!v)return;
 if(e->n==e->cap){e->cap=e->cap?e->cap*2:256;e->paths=realloc(e->paths,e->cap*sizeof(*e->paths));e->values=realloc(e->values,e->cap*sizeof(*e->values));need(e->paths&&e->values,"RSTB estimates");}
 e->paths[e->n]=strdup(path);need(e->paths[e->n]!=NULL,"RSTB estimates");e->values[e->n++]=v;
}
typedef struct {const char *name;const uint8_t *data;size_t n;} Member;
typedef struct {Member *m;size_t n,cap;} Members;
static void member_add(void *ctx,const char *name,const uint8_t *data,size_t n) {
 Members *ms=ctx;if(ms->n==ms->cap){ms->cap=ms->cap?ms->cap*2:64;ms->m=realloc(ms->m,ms->cap*sizeof(*ms->m));need(ms->m!=NULL,"SARC members");}
 ms->m[ms->n++]=(Member){name,data,n};
}
static int ends(const char *s,const char *t) {size_t a=strlen(s),b=strlen(t);return a>=b&&!strcmp(s+a-b,t);}
/* One player file's entries: the file itself, and for an archive (.pack / .sarc) each member that differs from the game's. */
static void estimate_file(Estimates *e,const Overlay *o,uint64_t vanilla,unsigned *unread) {
 const char *p=o->name;if(!mods_rstb_estimate(p,1,NULL,0))return;   /* streams, the table itself */
 int zs=ends(p,".zs"),pack=ends(p,".pack.zs")||ends(p,".pack")||ends(p,".sarc.zs")||ends(p,".sarc"),data=mods_rstb_needs_data(p);
 if(!zs&&!data&&!pack){est_add(e,p,mods_rstb_estimate(p,o->size,NULL,0));return;}
 if(ends(p,".mc")){uint8_t h[16]={0};int fd=open(o->path,O_RDONLY|O_CLOEXEC);ssize_t r=fd>=0?pread(fd,h,sizeof h,0):-1;if(fd>=0)close(fd);
  est_add(e,p,mods_rstb_estimate(p,o->size,r==16?h:NULL,r==16?16:0));return;}
 if(zs&&!data&&!pack){uint8_t h[32];int fd=open(o->path,O_RDONLY|O_CLOEXEC);ssize_t r=fd>=0?pread(fd,h,sizeof h,0):-1;if(fd>=0)close(fd);
  unsigned long long size=r>0?ZSTD_getFrameContentSize(h,(size_t)r):ZSTD_CONTENTSIZE_ERROR;
  if(size!=ZSTD_CONTENTSIZE_ERROR&&size!=ZSTD_CONTENTSIZE_UNKNOWN){est_add(e,p,mods_rstb_estimate(p,size,NULL,0));return;}}
 size_t n=0,m=0;uint8_t *raw=host_file(o->path,&n,512u<<20),*plain=raw;
 if(raw&&zs){plain=zs_decompress(raw,n,&m);free(raw);raw=NULL;n=m;}
 if(!plain){(*unread)++;est_add(e,p,mods_rstb_estimate(p,o->size*4,NULL,0));return;}   /* unreadable: a generous guess from the size */
 est_add(e,p,mods_rstb_estimate(p,n,plain,n));
 if(pack){Members mine={0},orig={0};uint8_t *vz=NULL,*vp=NULL;size_t vn=0,vm=0;
  if(vanilla!=ADDED){vz=image_file(vanilla,&vn);vp=zs?zs_decompress(vz,vn,&vm):vz;if(zs){free(vz);vz=NULL;}else vm=vn;if(vp)sarc_each(vp,vm,member_add,&orig);}
  if(sarc_each(plain,n,member_add,&mine))
   for(size_t k=0;k<mine.n;k++){int same=0;for(size_t j=0;j<orig.n&&!same;j++)same=!strcmp(orig.m[j].name,mine.m[k].name)&&orig.m[j].n==mine.m[k].n&&!memcmp(orig.m[j].data,mine.m[k].data,mine.m[k].n);
    if(!same&&*mine.m[k].name)est_add(e,mine.m[k].name,mods_rstb_estimate(mine.m[k].name,mine.m[k].n,mine.m[k].data,mine.m[k].n));}
  free(mine.m);free(orig.m);free(vp);}
 free(plain);
}
static void rstb_serve(size_t from) {
 /* The table's RomFS path: System/Resource/ResourceSizeTable.Product.<v>[.Nin_NX_NVN].rsizetable.zs */
 const char *prefix="System/Resource/ResourceSizeTable.Product.";uint64_t ri=lower(prefix);
 if(ri>=file_count||strncmp(entry_name(entry(ri)),prefix,strlen(prefix))||!ends(entry_name(entry(ri)),".rsizetable.zs"))return;
 const char *rname=entry_name(entry(ri));
 for(size_t k=from;k<noverlay;)if(overlays[k].index==ri){fprintf(stderr,"[totk] mods: %s from %s left out: the game's table is adjusted instead\n",rname,overlays[k].path);free(overlays[k].path);free(overlays[k].name);overlays[k]=overlays[--noverlay];}else k++;
 if(noverlay==from)return;
 for(size_t k=0;k<from;k++)if(overlays[k].index==ri){fprintf(stderr,"[totk] mods: the build replaces %s; not adjusted for mods\n",rname);return;}
 uint64_t t0=runtime_now_ns();
 /* The key: what the table depends on. */
 CC_SHA256_CTX c;CC_SHA256_Init(&c);char tag[128];snprintf(tag,sizeof tag,"totk-mods-rstb-1\n%s\n",ROMFS_IMAGE_SHA256);CC_SHA256_Update(&c,tag,(CC_LONG)strlen(tag));   /* the hash is read at start now (game_data.h) */
 for(size_t k=from;k<noverlay;k++){const Overlay *o=&overlays[k];int64_t id[5]={(int64_t)o->size,(int64_t)o->dev,(int64_t)o->ino,(int64_t)o->mtime.tv_sec,(int64_t)o->mtime.tv_nsec};
  CC_SHA256_Update(&c,o->name,(CC_LONG)strlen(o->name)+1);CC_SHA256_Update(&c,id,sizeof id);}
 uint8_t digest[32];char key[65];CC_SHA256_Final(digest,&c);for(unsigned k=0;k<32;k++)snprintf(key+k*2,3,"%02x",digest[k]);
 char cache[4096],file[4200],keyfile[4200];snprintf(cache,sizeof cache,"%s/.cache",mods_dir());
 if(mkdir(cache,0755)&&errno!=EEXIST){const char *t=getenv("TMPDIR");snprintf(cache,sizeof cache,"%s",t&&*t?t:"/tmp");}
 snprintf(file,sizeof file,"%s/ResourceSizeTable-" TOTK_GAME_DIR ".rsizetable.zs",cache);snprintf(keyfile,sizeof keyfile,"%s.key",file);
 char saved[80]={0};FILE *kf=fopen(keyfile,"r");if(kf){if(!fgets(saved,sizeof saved,kf))*saved=0;fclose(kf);}
 struct stat st;int reuse=!strncmp(saved,key,64)&&!stat(file,&st)&&S_ISREG(st.st_mode)&&st.st_size>0;
 if(!reuse){
  size_t zn,tn=0;uint8_t *z=image_file(ri,&zn),*table=zs_decompress(z,zn,&tn);free(z);
  if(!table){fprintf(stderr,"[totk] mods: cannot read %s; the game's table is used unchanged\n",rname);return;}
  Estimates e={0};unsigned unread=0;
  for(size_t k=from;k<noverlay;k++){uint64_t v=overlays[k].index;estimate_file(&e,&overlays[k],v,&unread);}
  size_t pn=0;ModsRstbStats s;uint8_t *patched=mods_rstb_patch(table,tn,(const char *const *)e.paths,e.values,e.n,&pn,&s);free(table);
  for(size_t k=0;k<e.n;k++)free(e.paths[k]);free(e.paths);free(e.values);
  if(!patched){fprintf(stderr,"[totk] mods: %s is not a RESTBL table; the game's table is used unchanged\n",rname);return;}
  size_t fn;uint8_t *frame=mods_zstd_raw_frame(patched,pn,&fn);free(patched);need(frame!=NULL,"RSTB frame");
  char tmp[4300];snprintf(tmp,sizeof tmp,"%s.%ld.tmp",file,(long)getpid());int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
  int ok=fd>=0&&write(fd,frame,fn)==(ssize_t)fn;if(fd>=0)ok&=!close(fd);free(frame);
  if(!ok||rename(tmp,file)){unlink(tmp);fprintf(stderr,"[totk] mods: cannot write %s (%s); the game's table is used unchanged\n",file,strerror(errno));return;}
  kf=fopen(keyfile,"w");if(kf){fprintf(kf,"%s\n",key);fclose(kf);}
  fprintf(stderr,"[totk] mods: ResourceSizeTable: %zu entries from %zu player files: %u raised, %u added, %u already large enough%s (%.2f s)\n",
   (size_t)(s.raised+s.added+s.kept),noverlay-from,s.raised,s.added,s.kept,unread?", some files unreadable (guessed from their size)":"",(runtime_now_ns()-t0)/1e9);
  need(stat(file,&st)==0,"RSTB cache stat");
 } else fprintf(stderr,"[totk] mods: ResourceSizeTable: the adjusted table for these files is cached (%s)\n",file);
 push_overlay(ri,file,rname,&st);
 fprintf(stderr,"[totk] mods: %s is served adjusted (%lld bytes, zstd raw blocks)\n",rname,(long long)st.st_size);
}
/* TOTK_ROMFS_PATCHES (default extracted/<version>/romfs-patches; 0 = none); with a true-2K output bigger than 1920x1080
   or a scene reserve above 1600x900 (1.4.3) also <TOTK_ROMFS_PATCHES_OUT, default extracted/<version>/romfs-patches-out>/
   <w>x<h>[-scene-<sw>x<sh>] (the HeapDef sizes, scripts/make_output_res_overlay.py; runtime_heap_overlay names it); then the player's older folder TOTK_ROMFS_MODS (0 = none) and the mods folder (TOTK_MODS_DIR, 0 = none). */
static void overlay_init(void) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0,0);
 npatch=noverlay;
 /* A canvas above 1080p and a scene reserve above 1600x900 (1.4.3) grow the heaps: the overlay <w>x<h>[-scene-<sw>x<sh>]. */
 char name[64];if(runtime_heap_overlay(name,sizeof name)){const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096];
  need(snprintf(d,sizeof(d),"%s/%s",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",name)<(int)sizeof(d),"RomFS output overlay path");
  /* build_app.py makes the 2560x1440 canvas and, for 1.4.3, the 1920x1080 / 2560x1440 scenes on either canvas; other sizes are one command. */
  char m[4200];snprintf(m,sizeof(m),"%s/manifest.tsv",d);
  if(access(m,R_OK)){char canvas[64];snprintf(canvas,sizeof canvas,"%s",name);char *scene=strstr(canvas,"-scene-");if(scene){*scene=0;scene+=7;}
   fprintf(stderr,"[totk] RomFS overlay %s missing: make it with\n  .venv/bin/python scripts/make_output_res_overlay.py %s%s%s --version " TOTK_GAME_DIR "%s%s\n",d,canvas,scene?" --scene ":"",scene?scene:"",out?" --output-root ":"",out?out:"");
   runtime_fail("HeapDef overlay for this output / scene size missing (see the line above)");}
  overlay_load(d,1,0);}
 nbuild=noverlay;
 const char *mods=getenv("TOTK_ROMFS_MODS");if(mods&&*mods&&strcmp(mods,"0"))overlay_load(mods,0,1);
 mods_load();
 rstb_serve(nbuild);
 added_init();
 if(noverlay)qsort(overlays,noverlay,sizeof(*overlays),overlay_order);
 for(size_t k=1;k<noverlay;k++)if(overlays[k].index==overlays[k-1].index){
  fprintf(stderr,"[totk] RomFS overlay: %s and %s replace the same file %s\n",overlays[k-1].path,overlays[k].path,overlays[k].name);
  runtime_fail("RomFS overlay: two overlays replace the same file (see the log; remove it from one of the manifests)");}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 romfs_image_size=runtime_game_number("romfs-config.txt","ROMFS_IMAGE_SIZE");romfs_image_sha256=runtime_game_sha256("romfs-config.txt","ROMFS_IMAGE_SHA256");
 romfs_index_sha256=runtime_game_sha256("romfs-config.txt","ROMFS_INDEX_SHA256");
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");runtime_boot_progress("Reading the data index",0,0);sha_fd(fd,ROMFS_INDEX_SHA256,0,NULL);
 index_size=(size_t)st.st_size;index_bytes=mmap(NULL,index_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(index_bytes!=MAP_FAILED,"map RomFS index");
 need(!memcmp(index_bytes,"TOTKRFS1",8),"RomFS index format");memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
 need(file_count<1000000&&strings_offset==24+file_count*24&&strings_offset<index_size,"RomFS index layout");
 const char *previous=NULL;
 for(uint64_t i=0;i<file_count;i++){Entry e=entry(i);need(e.offset<=ROMFS_IMAGE_SIZE&&e.size<=ROMFS_IMAGE_SIZE-e.offset,"RomFS file bounds");need(e.name<index_size-strings_offset&&e.len>1&&e.len<=index_size-strings_offset-e.name,"RomFS name bounds");const char *name=entry_name(e);need(name[e.len-1]==0&&strlen(name)==e.len-1&&(!previous||strcmp(previous,name)<0),"RomFS sorted paths");previous=name;}
 /* TOTK_ROMFS_DATA: the folder of the image (romfs.bin or its parts) when it is not the index's: the phone keeps the 17 GB in
    Documents/GameData (ios/Sources/main.m), so a new build of the app carries only the index and installs in minutes. */
 const char *data=getenv("TOTK_ROMFS_DATA");if(!data||!*data)data=dir;
 need(snprintf(path,sizeof(path),"%s/romfs.bin",data)<(int)sizeof(path),"RomFS image path");image_fd=open(path,O_RDONLY);
 if(image_fd<0)open_chunks(data);
 else need(fstat(image_fd,&st)==0&&(uint64_t)st.st_size==ROMFS_IMAGE_SIZE,"RomFS image size");
 verify_image(image_fd,dir);
 if(!nchunks&&getenv("TOTK_ROMFS_MMAP")&&strcmp(getenv("TOTK_ROMFS_MMAP"),"0")){
  void *mapped=mmap(NULL,(size_t)ROMFS_IMAGE_SIZE,PROT_READ,MAP_PRIVATE,image_fd,0);
  if(mapped!=MAP_FAILED){image_map=mapped;fprintf(stderr,"[totk] RomFS mmap read path enabled\n");}
  else fprintf(stderr,"[totk] RomFS mmap failed (%s); using pread\n",strerror(errno));
 }
 runtime_boot_status("Preparing game data","Checking the resolution patch and mounting the read-only game data");
 overlay_init();
 fprintf(stderr,"[totk] native read-only RomFS ready: %llu files\n",(unsigned long long)file_count);
}
static int path_ok(const char *p) {return romfs_path_valid_relative(p);}
static const char *rom_path(const char *path) {
 return romfs_path_relative(mount_name,path);
}
static uint64_t lower(const char *name) {uint64_t lo=0,hi=file_count;while(lo<hi){uint64_t m=lo+(hi-lo)/2;if(strcmp(entry_name(entry(m)),name)<0)lo=m+1;else hi=m;}return lo;}
static unsigned mount_rom(const char *name,void *cache,uint64_t size) {
 (void)cache;(void)size;runtime_fs_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(mount_name)||strchr(name,'/')||strchr(name,':'))return INVALID;
 if(*mount_name&&strcmp(name,mount_name))runtime_fail("second RomFS mount not implemented");strcpy(mount_name,name);return 0;
}
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p=rom_path(path);if(!p)return NOT_FOUND;uint64_t i=file_index(p);if(i==UINT64_MAX)return NOT_FOUND;
 runtime_autoshrine_romfs(p);   /* auto shrine watches which map loads (world_survey.c) */
 uint32_t ov=overlay_slot(i);int fd=-1;
 if(ov){const Overlay *o=&overlays[ov-1];fd=open(o->path,O_RDONLY|O_CLOEXEC);struct stat st;
  if(fd<0||fstat(fd,&st)!=0||(uint64_t)st.st_size!=o->size||st.st_dev!=o->dev||st.st_ino!=o->ino||st.st_mtimespec.tv_sec!=o->mtime.tv_sec||st.st_mtimespec.tv_nsec!=o->mtime.tv_nsec){
   fprintf(stderr,"[totk] RomFS overlay: %s is missing or changed since the start check\n",o->path);runtime_fail("RomFS overlay file changed while the game ran (restart the game)");}}
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;handle_fd[j]=fd;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=i-1<file_count?entry(i-1):(Entry){0};return 1;}   /* an added file's bytes are all in its overlay */
/* Where an open handle's bytes come from: the overlay file, or the original's range of the image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size) {
 pthread_mutex_lock(&lock);uint32_t ov=handle_ovl[h-0x100000000ULL];int ofd=handle_fd[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=ofd;*base=0;*size=overlays[ov-1].size;}else{*fd=image_fd;*base=e->offset;*size=e->size;}
}
unsigned runtime_romfs_read(uint64_t *out,uint64_t h,int64_t offset,void *buffer,uint64_t size) {
 Entry e;if(!out||offset<0||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,fsize;handle_source(h,&e,&fd,&base,&fsize);if((uint64_t)offset>fsize||(!buffer&&size))return INVALID;
 uint64_t n=size;if(n>fsize-(uint64_t)offset)n=fsize-(uint64_t)offset;uint64_t done=0;
 static _Atomic int profile_mode=-1;int profile=atomic_load(&profile_mode);if(profile<0){profile=getenv("TOTK_FS_TIMING")!=NULL;atomic_store(&profile_mode,profile);}uint64_t prep_start=profile?runtime_now_ns():0;
 dm_guest_write_prepare(buffer,(size_t)n);
 uint64_t t0=profile?runtime_now_ns():0;
 if(fd==image_fd&&image_map){memcpy(buffer,image_map+base+(uint64_t)offset,(size_t)n);done=n;}
 else if(fd==image_fd&&n&&n<BLK&&block_cache_on()){   /* one or two blocks */
  uint64_t off=base+(uint64_t)offset,split=(off/BLK+1)*BLK;size_t first=off+n>split?(size_t)(split-off):(size_t)n;
  block_read(buffer,off,first);if(first<n)block_read((uint8_t *)buffer+first,split,(size_t)n-first);done=n;}
 else while(done<n){size_t chunk=(size_t)(n-done);if(chunk>8<<20)chunk=8<<20;ssize_t r=src_pread(fd,(uint8_t *)buffer+done,chunk,(off_t)(base+(uint64_t)offset+done));if(r<=0)runtime_fail("RomFS read failure");done+=(uint64_t)r;}
 if(profile){uint64_t now=runtime_now_ns();atomic_fetch_add(&read_calls,1);atomic_fetch_add(&read_bytes,done);atomic_fetch_add(&read_ns,now-t0);atomic_fetch_add(&read_prepare_ns,t0-prep_start);
  uint64_t last=atomic_load(&read_report_ns);
  if(now-last>1000000000ULL&&atomic_compare_exchange_strong(&read_report_ns,&last,now))
   fprintf(stderr,"[totk-fs] read window t=%.3f calls=%llu bytes=%llu wall_ms=%.3f prepare_ms=%.3f\n",now/1e9,
    (unsigned long long)atomic_exchange(&read_calls,0),(unsigned long long)atomic_exchange(&read_bytes,0),atomic_exchange(&read_ns,0)/1e6,atomic_exchange(&read_prepare_ns,0)/1e6);}
 *out=done;return 0;
}
unsigned runtime_romfs_size(int64_t *out,uint64_t h) {Entry e;if(!out||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,size;handle_source(h,&e,&fd,&base,&size);*out=(int64_t)size;return 0;}
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");uint64_t j=h-0x100000000ULL;pthread_mutex_lock(&lock);int fd=handle_ovl[j]?handle_fd[j]:-1;handles[j]=0;handle_ovl[j]=0;handle_fd[j]=-1;pthread_mutex_unlock(&lock);if(fd>=0)close(fd);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p=rom_path(path);if(!p)return NOT_FOUND;if(file_index(p)!=UINT64_MAX){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;if(folder_exists(dir)){*out=0;return 0;}return NOT_FOUND;
}
int runtime_romfs_owns(const char *path) {size_t n=strlen(mount_name);return n&&path&&!strncmp(path,mount_name,n)&&path[n]==':';}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
/* Host-side reads for the shared shader list (third_party/d2r_gfx/gfx/nvn/totk_shader_list.m): it finds the shaders a
   downloaded list names in the player's own game files on a background thread, without the guest's file handles. The
   bytes are what the game itself reads (a mod's copy where one replaces a file). */
const char *runtime_game_dir(void) {return TOTK_GAME_DIR;}   /* the game's modules are rewritten/<this>/ (relative to the game folder) */
size_t runtime_romfs_file_count(void) {return image_fd<0?0:(size_t)file_count;}
int runtime_romfs_has_file(const char *p) {return image_fd>=0&&p&&file_index(p)!=UINT64_MAX;}   /* a RomFS path (no mount name), the image's or a mod's added file */
const char *runtime_romfs_file_name(size_t i) {return image_fd>=0&&i<file_count?entry_name(entry(i)):NULL;}
uint8_t *runtime_romfs_host_read(const char *p,size_t *n) {
 if(image_fd<0||!p||!n||!path_ok(p))return NULL;uint64_t i=file_index(p);if(i==UINT64_MAX)return NULL;
 uint32_t ov=overlay_slot(i);if(ov)return host_file(overlays[ov-1].path,n,(size_t)1<<30);
 return i<file_count?image_file(i,n):NULL;
}
static pthread_once_t dicts_once=PTHREAD_ONCE_INIT;
static void dicts_load_once(void) {load_dicts();}
uint8_t *runtime_romfs_zs_decompress(const uint8_t *src,size_t n,size_t *out) {
 if(image_fd<0||!src||n<4||!out)return NULL;pthread_once(&dicts_once,dicts_load_once);return zs_decompress(src,n,out);
}
/* This RomFS with its player layers: the index digest the build verified and each overlay's place, size and modification
   time. A shader index made from other files (a mod added or removed) is not reused. */
void runtime_romfs_identity(char out[65]) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);CC_SHA256_Update(&c,ROMFS_INDEX_SHA256,(CC_LONG)strlen(ROMFS_INDEX_SHA256));
 for(size_t k=0;k<noverlay;k++){char line[160];int l=snprintf(line,sizeof line,"\n%llu\t%llu\t%lld.%09ld\t",(unsigned long long)overlays[k].index,(unsigned long long)overlays[k].size,(long long)overlays[k].mtime.tv_sec,(long)overlays[k].mtime.tv_nsec);
  CC_SHA256_Update(&c,line,(CC_LONG)l);CC_SHA256_Update(&c,overlays[k].name,(CC_LONG)strlen(overlays[k].name));}
 unsigned char d[32];CC_SHA256_Final(d,&c);for(int i=0;i<32;i++)snprintf(out+2*i,3,"%02x",d[i]);
}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size}};
 for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(name,map[i].name))return map[i].fn;return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
 runtime_fs_init();need(mount_rom("rom",NULL,0)==0,"mount RomFS");unsigned count=0;uint8_t buffer[257];uint64_t sum=0;
 /* Audio stream paths include an extra slash after the mount root. Verify
    actual resource lookup and bytes, while still rejecting path traversal. */
 {
  const char *name="Sound/Resource/Stream/Dm_OP_0001.bwav";
  char path[256];uint8_t expected[257],actual[257];uint64_t h,n;
  snprintf(path,sizeof path,"rom:/%s",name);
  need(!runtime_romfs_open(&h,path,1),"open normal audio path");
  need(!runtime_romfs_read(&n,h,0,expected,sizeof expected)&&n==sizeof expected,"read normal audio path");runtime_romfs_close(h);
  for(int slashes=2;slashes<=3;slashes++){
   snprintf(path,sizeof path,slashes==2?"rom://%s":"rom:///%s",name);
   int type=-1;need(!runtime_romfs_entry_type(&type,path)&&type==1,"normalized audio entry type");
   need(!runtime_romfs_open(&h,path,1),"open normalized audio stream path");
   need(!runtime_romfs_read(&n,h,0,actual,sizeof actual)&&n==sizeof actual&&!memcmp(actual,expected,sizeof actual),"normalized audio stream bytes");runtime_romfs_close(h);
  }
  need(runtime_romfs_open(&h,"rom://../Sound/Resource/Stream/Dm_OP_0001.bwav",1)!=0,"reject traversal after repeated root separator");
 }
 for(uint64_t i=0;i<file_count;i+=file_count/64){Entry e=entry(i);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;int64_t size;need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==e.size,"probe open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer)),"probe partial read");for(uint64_t j=0;j<n;j++)sum=sum*131+buffer[j];need(!runtime_romfs_read(&n,h,size,buffer,1)&&n==0,"probe EOF");runtime_romfs_close(h);count++;}
 for(size_t k=0;k<noverlay;k++){char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",overlays[k].name)<(int)sizeof(p),"probe overlay path");
  if(overlays[k].index>=file_count){int type=-1;char *slash=strchr(overlays[k].name,'/')?strrchr(p,'/'):NULL;
   if(slash){*slash=0;need(!runtime_romfs_entry_type(&type,p)&&type==0,"probe added file's folder");*slash='/';}
   need(!runtime_romfs_entry_type(&type,p)&&type==1,"probe added file type");}uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");uint64_t tail=overlays[k].size<64?overlays[k].size:64;int fd=open(overlays[k].path,O_RDONLY|O_CLOEXEC);
  need(fd>=0&&!runtime_romfs_read(&n,oh,size-(int64_t)tail,a,tail)&&n==tail&&pread(fd,b,tail,(off_t)(size-(int64_t)tail))==(ssize_t)tail&&!memcmp(a,b,tail),"probe overlay bytes");close(fd);runtime_romfs_close(oh);}
 /* the block cache: small reads (some across a block edge) equal direct reads of the image */
 unsigned cached=0;
 if(block_cache_on()&&!image_map)for(unsigned k=0,x=0x9e3779b9u;k<2000;k++){
  x=x*1664525u+1013904223u;Entry e=entry((uint64_t)x%file_count);if(!e.size||overlay_slot((uint64_t)x%file_count))continue;
  x=x*1664525u+1013904223u;uint64_t off=e.size>1?(uint64_t)x%e.size:0;
  x=x*1664525u+1013904223u;uint64_t len=1+(uint64_t)x%(k%4?4096:BLK-1);if(len>e.size-off)len=e.size-off;
  if(k%8==0&&e.size>2){uint64_t edge=(e.offset/BLK+1)*BLK;if(edge>e.offset&&edge<e.offset+e.size){off=edge-e.offset>1?edge-e.offset-1:0;len=e.size-off<3?e.size-off:3;}}
  char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;
  uint8_t *a=malloc(len),*b=malloc(len);need(a&&b&&!runtime_romfs_open(&h,p,1)&&!runtime_romfs_read(&n,h,(int64_t)off,a,len)&&n==len,"probe cached read");runtime_romfs_close(h);
  for(uint64_t got=0;got<len;){ssize_t r=src_pread(image_fd,b+got,(size_t)(len-got),(off_t)(e.offset+off+got));need(r>0,"probe direct read");got+=(uint64_t)r;}
  need(!memcmp(a,b,len),"RomFS block cache bytes equal the image");free(a);free(b);cached++;}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 printf("{\"status\":\"pass\",\"cached_reads_checked\":%u,\"files\":%llu,\"sampled_files\":%u,\"overlays\":%zu,\"added\":%zu,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, block cache against direct reads, verified overlays and added files; no game startup\"}\n",cached,(unsigned long long)file_count,count,noverlay,nadded,(unsigned long long)sum);
}
