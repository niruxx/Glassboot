// Sparse in-memory disk: exercises large layouts without allocating terabytes.
#include "Gpt.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
using namespace glassboot;
static int failures=0;
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while(0)
class SparseDisk : public BlockDevice {
public:
    explicit SparseDisk(uint64_t size, uint32_t ss=512):size_(size),sector_(ss) {}
    uint64_t sizeBytes() const override { return size_; }
    uint32_t logicalSectorSize() const override { return sector_; }
    void write(uint64_t at,const void* data,size_t n) override {
        if(at>size_ || n>size_-at || at%sector_ || n%sector_) throw Error("out of range or unaligned write");
        auto* p=static_cast<const uint8_t*>(data);
        for(size_t i=0;i<n;i++) bytes[at+i]=p[i];
    }
    void read(uint64_t at,void* data,size_t n) override {
        if(at>size_ || n>size_-at || at%sector_ || n%sector_) throw Error("out of range or unaligned read");
        auto* p=static_cast<uint8_t*>(data);
        for(size_t i=0;i<n;i++) { auto it=bytes.find(at+i); p[i]=it==bytes.end()?0:it->second; }
    }
    void flush() override {}
    std::map<uint64_t,uint8_t> bytes;
private:
    uint64_t size_;
    uint32_t sector_;
};
int main() {
    const uint64_t GiB=1ull<<30, MiB=1ull<<20, TiB=1ull<<40;
    for(uint64_t size:{GiB,3*TiB}) {
        auto l=computeGptLayout(size,64,128*MiB);
        CHECK(l.dataStart==2048);
        CHECK(l.espStart==l.dataStart+l.dataSectors);
        CHECK(l.espSectors==64*2048);
        CHECK(size/512-33-l.espStart-l.espSectors>=128*2048);
        if(size>2*TiB) CHECK(l.espStart>0xFFFFFFFFull);
        SparseDisk disk(size);
        writeGpt(disk,l);
        Layout parsed;
        CHECK(readGpt(disk,parsed));
        CHECK(parsed.dataSectors==l.dataSectors && parsed.espStart==l.espStart);
        disk.bytes[512+24]^=1; CHECK(!readGpt(disk,parsed)); disk.bytes[512+24]^=1;
        disk.bytes[1024+56]^=1; CHECK(!readGpt(disk,parsed)); disk.bytes[1024+56]^=1;
        disk.bytes[size-512+16]^=1; CHECK(!readGpt(disk,parsed)); disk.bytes[size-512+16]^=1;
        disk.bytes[size-33*512+56]^=1; CHECK(!readGpt(disk,parsed));
    }
    for(uint64_t reserved:{GiB,std::numeric_limits<uint64_t>::max()}) {
        bool threw=false;try {computeGptLayout(GiB,64,reserved);}catch(const Error&){threw=true;}CHECK(threw);
        threw=false;try {Layout::compute(GiB,64,reserved);}catch(const Error&){threw=true;}CHECK(threw);
    }
    auto mbr=Layout::compute(4*GiB,64,GiB);
    CHECK((mbr.espStart+mbr.espSectors)*512<=3*GiB);
    auto large=Layout::compute(3*TiB,64);
    CHECK(large.espStart+large.espSectors<=0xFFFFFFFFull);
    auto odd=computeGptLayout(GiB+511,64,MiB+1);
    CHECK(odd.espStart+odd.espSectors+(MiB+512)/512<=odd.totalSectors-33);
    for (uint64_t size : {GiB, 3*TiB}) {
        auto l=computeGptLayout(size,64,128*MiB,4096);
        SparseDisk disk(size,4096);
        writeGpt(disk,l); Layout parsed;
        CHECK(readGpt(disk,parsed));
        CHECK(parsed.dataStart==l.dataStart && parsed.espStart==l.espStart);
        CHECK(parsed.espStart+parsed.espSectors+128*2048<=size/512-40);
        disk.bytes[4096+24]^=1; CHECK(!readGpt(disk,parsed)); disk.bytes[4096+24]^=1;
        disk.bytes[size-4096+16]^=1; CHECK(!readGpt(disk,parsed));
    }
    auto raw=std::make_unique<SparseDisk>(MiB,4096);
    auto view=sectorView(std::move(raw),4096);
    AlignedBuffer all(4096),part(512); std::memset(all.data(),0x5a,all.size());
    view->write(0,all.data(),all.size()); std::memset(part.data(),0xa5,512);
    view->write(512,part.data(),512); view->read(0,all.data(),4096);
    CHECK(all.data()[0]==0x5a && all.data()[512]==0xa5 && all.data()[1024]==0x5a);
    std::printf(failures?"%d failures\n":"all layout tests passed\n",failures);
    return failures?1:0;
}
