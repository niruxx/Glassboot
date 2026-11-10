#include "Gpt.h"
#include <algorithm>
#include <cstring>
#include <random>

namespace glassboot {
namespace {
constexpr size_t tableBytes = 128 * 128;
const uint8_t basicData[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
const uint8_t espType[16] = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;i++) p[i]=uint8_t(v>>(8*i)); }
void put64(uint8_t* p, uint64_t v) { for (int i=0;i<8;i++) p[i]=uint8_t(v>>(8*i)); }
uint32_t get32(const uint8_t* p) { uint32_t v=0; for(int i=3;i>=0;i--) v=v<<8|p[i]; return v; }
uint64_t get64(const uint8_t* p) { uint64_t v=0; for(int i=7;i>=0;i--) v=v<<8|p[i]; return v; }
uint32_t crc(const uint8_t* p, size_t n) {
    uint32_t c=~0u;
    while(n--) { c^=*p++; for(int i=0;i<8;i++) c=(c>>1)^(0xEDB88320u & (0u-(c&1))); }
    return ~c;
}
void guid(uint8_t* p) {
    std::random_device rd;
    for(int i=0;i<16;i++) p[i]=uint8_t(rd());
    p[7]=(p[7]&15)|0x40; p[8]=(p[8]&63)|0x80;
}
void entry(uint8_t* p, const uint8_t* type, uint64_t start, uint64_t sectors, const char* name) {
    std::memcpy(p,type,16); guid(p+16);
    put64(p+32,start); put64(p+40,start+sectors-1);
    for(size_t i=0;name[i] && i<36;i++) p[56+2*i]=uint8_t(name[i]);
}
void header(uint8_t* p, uint64_t at, uint64_t other, uint64_t end, uint64_t table,
            const uint8_t* diskGuid, uint32_t tableCrc) {
    std::memcpy(p,"EFI PART",8); put32(p+8,0x10000); put32(p+12,92);
    put64(p+24,at); put64(p+32,other); put64(p+40,34); put64(p+48,end-33);
    std::memcpy(p+56,diskGuid,16); put64(p+72,table);
    put32(p+80,128); put32(p+84,128); put32(p+88,tableCrc); put32(p+16,crc(p,92));
}
bool validHeader(uint8_t* p, uint64_t at, uint64_t other, uint64_t table, uint64_t end) {
    if(std::memcmp(p,"EFI PART",8) || get32(p+8)!=0x10000 || get32(p+12)!=92 || get32(p+20)!=0 ||
       get64(p+24)!=at || get64(p+32)!=other || get64(p+40)!=34 || get64(p+48)!=end-33 ||
       get64(p+72)!=table || get32(p+80)!=128 || get32(p+84)!=128) return false;
    uint32_t want=get32(p+16); put32(p+16,0);
    bool ok=crc(p,92)==want; put32(p+16,want); return ok;
}
}
Layout computeGptLayout(uint64_t diskBytes, uint32_t espMb, uint64_t reserveBytes) {
    Layout l;
    l.totalSectors=diskBytes/512;
    l.dataStart=2048;
    l.espSectors=uint64_t(espMb)*2048;
    if(reserveBytes>diskBytes || l.totalSectors<34) throw Error("disk too small for GPT and reserved space");
    uint64_t reserved=reserveBytes/512+(reserveBytes%512!=0);
    uint64_t available=l.totalSectors-33;
    if(reserved>available || available-reserved<l.dataStart+l.espSectors+256ull*2048)
        throw Error("disk too small for GPT, data, ESP and reserved space");
    l.dataSectors=(available-reserved-l.dataStart-l.espSectors)/2048*2048;
    l.espStart=l.dataStart+l.dataSectors;
    return l;
}
void writeGpt(BlockDevice& dev, const Layout& l) {
    AlignedBuffer table(tableBytes), primary(512), backup(512), mbr(512);
    table.clear(); primary.clear(); backup.clear(); mbr.clear();
    entry(table.data(),basicData,l.dataStart,l.dataSectors,"GLASSBOOT");
    entry(table.data()+128,espType,l.espStart,l.espSectors,"GLASS_EFI");
    uint8_t diskGuid[16]; guid(diskGuid);
    uint64_t end=l.totalSectors-1;
    uint32_t sum=crc(table.data(),tableBytes);
    header(primary.data(),1,end,end,2,diskGuid,sum);
    header(backup.data(),end,1,end,end-32,diskGuid,sum);
    uint8_t* e=mbr.data()+446; e[4]=0xEE; e[2]=2;
    e[5]=0xFF; e[6]=0xFF; e[7]=0xFF;
    put32(e+8,1); put32(e+12,uint32_t(std::min<uint64_t>(end,0xFFFFFFFFull)));
    mbr.data()[510]=0x55; mbr.data()[511]=0xAA;
    dev.write((end-32)*512,table.data(),tableBytes); dev.write(end*512,backup.data(),512);
    dev.write(1024,table.data(),tableBytes); dev.write(512,primary.data(),512);
    dev.write(0,mbr.data(),512);
}
bool readGpt(BlockDevice& dev, Layout& l) {
    uint64_t total=dev.sizeBytes()/512;
    if(total<4096) return false;
    uint64_t end=total-1;
    AlignedBuffer primary(512),backup(512),table(tableBytes),other(tableBytes),mbr(512);
    dev.read(0,mbr.data(),512);
    if(mbr.data()[510]!=0x55 || mbr.data()[511]!=0xAA || mbr.data()[450]!=0xEE ||
       get32(mbr.data()+454)!=1 || get32(mbr.data()+458)!=std::min<uint64_t>(end,0xFFFFFFFFull)) return false;
    dev.read(512,primary.data(),512); dev.read(end*512,backup.data(),512);
    if(!validHeader(primary.data(),1,end,2,end) || !validHeader(backup.data(),end,1,end-32,end) ||
       std::memcmp(primary.data()+56,backup.data()+56,16) || get32(primary.data()+88)!=get32(backup.data()+88)) return false;
    dev.read(1024,table.data(),tableBytes); dev.read((end-32)*512,other.data(),tableBytes);
    if(crc(table.data(),tableBytes)!=get32(primary.data()+88) || std::memcmp(table.data(),other.data(),tableBytes)) return false;
    const uint8_t *a=table.data(), *b=a+128;
    if(std::memcmp(a,basicData,16) || std::memcmp(b,espType,16)) return false;
    // Extra partitions are allowed in reserved space. Validate every occupied range.
    for(size_t i=0;i<128;i++) {
        const uint8_t* e=a+i*128;
        bool used=false; for(int k=0;k<16;k++) used|=e[k]!=0;
        if(!used) continue;
        uint64_t start=get64(e+32),stop=get64(e+40);
        if(start<34 || stop<start || stop>end-33) return false;
        // The boot gap contains our marker; only tail space is available to extras.
        if(i>=2 && start<=get64(b+40)) return false;
        for(size_t j=0;j<i;j++) {
            const uint8_t* prev=a+j*128; bool occupied=false;
            for(int k=0;k<16;k++) occupied|=prev[k]!=0;
            if(occupied && start<=get64(prev+40) && get64(prev+32)<=stop) return false;
        }
    }
    l={}; l.totalSectors=total; l.dataStart=get64(a+32); l.espStart=get64(b+32);
    l.dataSectors=get64(a+40)-l.dataStart+1; l.espSectors=get64(b+40)-l.espStart+1;
    return l.dataStart==2048 && l.dataSectors>=256ull*2048 && l.espSectors>=2048 &&
           l.espStart==l.dataStart+l.dataSectors;
}
} // namespace glassboot
