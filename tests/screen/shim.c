/* Host shim: run the core + the firmware's terminal emulator, feed scripted
 * keystrokes, dump virtual screens as binary for tests/screen/render.py.
 *   shim os8  disk.rk05 "script"
 *   shim tss8 rf.dsk init.bin "console script" "line0 script"
 * '~' in a script waits ~3 s of simulated time. */
#include "../../core/pdp8.h"
#include "../../firmware/term.h"
#include <stdio.h>
#include <string.h>
void video_flash(void) {}
static term_t T[5];
static FILE *d, *rf;
bool host_disk_present(int u){return u==0 && d;}
bool host_disk_readonly(int u){(void)u;return false;}
bool host_disk_read(int u,uint32_t s,uint16_t*w){(void)u;memset(w,0,512);fseek(d,s*512,SEEK_SET);if(fread(w,1,512,d)){};return true;}
bool host_disk_write(int u,uint32_t s,const uint16_t*w){(void)u;fseek(d,s*512,SEEK_SET);fwrite(w,1,512,d);return true;}
uint32_t host_rf_words(void){return rf?262144:0;}
bool host_rf_read(uint32_t b,uint16_t*w){memset(w,0,512);fseek(rf,b*512,SEEK_SET);if(fread(w,1,512,rf)){};return true;}
bool host_rf_write(uint32_t b,const uint16_t*w){fseek(rf,b*512,SEEK_SET);fwrite(w,1,512,rf);return true;}
void host_tty_out(uint8_t c){term_putc(&T[0],c);}
void host_ttx_out(int l,uint8_t c){term_putc(&T[l+1],c);}
void host_lpt_out(uint8_t c){(void)c;}
static uint32_t fake_ms;
uint32_t host_millis(void){return fake_ms;}
static void run(int n){for(int i=0;i<n;i++){pdp8_run(20000);fake_ms+=5;}}
static void feed(int line,const char*s){
  for(;*s;s++){char c=*s; if(c=='\n')c='\r'; if(c=='~'){run(600);continue;}
    if(line==0) pdp8_key((uint8_t)(c|0200)); else pdp8_ttx_key(line-1,(uint8_t)c); run(30);} }
static void dump(int n,const char*fn){FILE*o=fopen(fn,"wb");for(int y=0;y<TEXT_ROWS;y++)fwrite((void*)T[n].buf[y],2,TEXT_COLS,o);fclose(o);}
int main(int argc,char**argv){
  for(int i=0;i<5;i++) term_init(&T[i],i);
  if(!strcmp(argv[1],"os8")){ d=fopen(argv[2],"r+b"); pdp8_boot_rk(0); run(2000); feed(0,argv[3]); run(2000); dump(0,"screen.bin"); return 0; }
  rf=fopen(argv[2],"r+b"); static uint8_t tape[65536]; FILE*b=fopen(argv[3],"rb"); size_t n=fread(tape,1,sizeof tape,b); fclose(b);
  pdp8_reset(); printf("bin: %d words\n", pdp8_load_bin(tape,(uint32_t)n)); pdp8_start(024200);
  run(1000); feed(0,argv[4]); feed(1,argv[5]); run(2000);
  dump(0,"screen0.bin"); dump(1,"screen1.bin"); pdp8_rf_flush(); return 0;
}
