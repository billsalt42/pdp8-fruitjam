/* Host preview of the front panel: boots OS/8, lets it idle, shows the panel,
 * then exercises HALT / switches / ADDR LOAD / DEP / EXAM / SING STEP / CONT. */
#include "../../core/pdp8.h"
#include "../../firmware/panel.h"
#include <stdio.h>
#include <string.h>
void video_flash(void) {}
static term_t con;
static FILE *d;
bool host_disk_present(int u){return u==0 && d;}
bool host_disk_readonly(int u){(void)u;return false;}
bool host_disk_read(int u,uint32_t s,uint16_t*w){(void)u;memset(w,0,512);fseek(d,s*512,SEEK_SET);if(fread(w,1,512,d)){};return true;}
bool host_disk_write(int u,uint32_t s,const uint16_t*w){(void)u;fseek(d,s*512,SEEK_SET);fwrite(w,1,512,d);return true;}
uint32_t host_rf_words(void){return 0;}
bool host_rf_read(uint32_t b,uint16_t*w){(void)b;(void)w;return false;}
bool host_rf_write(uint32_t b,const uint16_t*w){(void)b;(void)w;return false;}
void host_tty_out(uint8_t c){term_putc(&con,c);}
void host_ttx_out(int l,uint8_t c){(void)l;(void)c;}
void host_lpt_out(uint8_t c){(void)c;}
int host_ptr_getc(void){return -1;}
void host_ptp_putc(uint8_t c){(void)c;}
static uint32_t ms;
uint32_t host_millis(void){return ms;}
static void run(int frames){ for(int f=0;f<frames;f++){ for(int i=0;i<8;i++){ if(!pdp8.halted) pdp8_run(50000); ms+=3; } panel_update(ms);} }
static void dump(const char*fn){FILE*o=fopen(fn,"wb");for(int y=0;y<TEXT_ROWS;y++)fwrite((void*)panel_screen()->buf[y],2,TEXT_COLS,o);fclose(o);}
static void keys(const char*s){ for(;*s;s++){ panel_key(*s); run(1);} }
int main(int argc,char**argv){
  (void)argc; term_init(&con,0); d=fopen(argv[1],"r+b");
  pdp8_boot_rk(0); run(300);                     /* OS/8 idling at the '.' prompt */
  panel_init(); panel_set_system("OS/8"); panel_show(true); run(40);
  dump("panel_run.bin");
  keys("H");                                      /* HALT */
  keys("O0200");                                  /* switches = 0200 */
  keys("L");                                      /* ADDR LOAD */
  keys("O7402"); keys("D");                       /* deposit HLT at 0200 */
  keys("O0200"); keys("L"); keys("E");            /* examine it */
  panel_key(PANEL_KEY_UP); panel_key(PANEL_KEY_UP); run(20);   /* selector -> MD */
  printf("after EXAM: M[0200]=%04o pc=%04o sr=%04o\n", pdp8_mem[0200], pdp8.pc, pdp8.sr);
  dump("panel_halt.bin");
  keys("S");                                      /* single step executes the HLT */
  printf("after SING STEP: pc=%04o halted=%d icount=%llu\n", pdp8.pc, pdp8.halted, (unsigned long long)pdp8.icount);
  keys("O7600"); keys("L"); keys("G"); run(100); /* back to the OS/8 monitor */
  printf("after CONT at 7600: halted=%d pc=%04o\n", pdp8.halted, pdp8.pc);
  return 0;
}
