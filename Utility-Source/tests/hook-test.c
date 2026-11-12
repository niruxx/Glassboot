// Include the early-boot helper to exercise the exact GPT matching code used at boot.
#define main glassboot_hook_entry
#include "../../hook/glassboot-hook.c"
#undef main
int main(int argc, char **argv) {
    const char *maps[] = {"GBMAP1 1536 2\n512 1024\n4096 512\n",
                          "GBMAP1 1536 2\n512 1024\n4096 511\n",
                          "GBMAP1 512 1\n51200 512\n",
                          "GBMAP1 512 1\n1 512\n",
                          "GBMAP1 512 65537\n", "GBMAP1 0 1\n0 512\n"};
    for (unsigned i=0; i<sizeof maps/sizeof maps[0]; i++) {
        FILE *f=tmpfile(); struct disk disk={.part_len=100}; struct runs r={0}; uint64_t size=0;
        if (!f || fwrite(maps[i],1,strlen(maps[i]),f)!=strlen(maps[i])) return 1;
        rewind(f); int rc=parse_file_map(&disk,f,&r,&size); fclose(f);
        if ((i==0 && (rc || size!=1536 || r.n!=2 || r.v[1].off!=4096)) || (i>0 && rc==0)) return 1;
        free(r.v);
    }
    uint8_t guid[16];
    const uint8_t expected[16]={0x33,0x22,0x11,0x00,0x55,0x44,0x77,0x66,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    if(parse_guid("00112233-4455-6677-8899-aabbccddeeff",guid) || memcmp(guid,expected,16)) return 1;
    if(parse_guid("00112233-4455-6677-8899-AABBCCDDEEFF",guid) || memcmp(guid,expected,16)) return 1;
    if(!parse_guid("00112233-4455-6677-8899-aabbccddeefg",guid)) return 1;
    if(!parse_guid("00112233x4455-6677-8899-aabbccddeeff",guid)) return 1;
    if(!parse_guid("00112233-4455-6677-8899-aabbccddeef",guid)) return 1;
    if(argc==2) {
        int fd=open(argv[1],O_RDONLY);
        uint8_t entry[128];uint64_t start,len;
        if(fd<0 || preadn(fd,entry,128,1024) || gpt_partition(fd,entry+16,0,&start,&len) || start!=2048) return 1;
        memset(guid,0,16);
        if(!gpt_partition(fd,guid,0,&start,&len)) return 1;
        close(fd);
    }
    puts("hook GPT identifier tests passed");
    return 0;
}
