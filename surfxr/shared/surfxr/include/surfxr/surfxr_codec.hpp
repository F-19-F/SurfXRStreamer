#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace surfxr::transport {
using Bytes=std::vector<std::uint8_t>;
[[noreturn]] inline void fail(const char* code){throw std::runtime_error(code);}
inline void require(bool v,const char* code){if(!v)fail(code);}
// SURFXR bincode 1 fixed LE and bincode 2 standard variable integers.
struct Writer {
    Bytes bytes;
    explicit Writer(bool modern=false):modern_(modern){}
    void raw_integer(std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)bytes.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
    void integer(std::uint64_t v,unsigned n){
        if(!modern_||n==1)raw_integer(v,n);
        else if(v<251)raw_integer(v,1);
        else if(v<=65535){raw_integer(251,1);raw_integer(v,2);}
        else if(v<=UINT32_MAX){raw_integer(252,1);raw_integer(v,4);}
        else{raw_integer(253,1);raw_integer(v,8);}
    }
    void boolean(bool v){integer(v,1);}
    void real(float v){require(std::isfinite(v),"surfxr_nonfinite_float");std::uint32_t bits;std::memcpy(&bits,&v,4);raw_integer(bits,4);}
    void duration(std::uint64_t ns){integer(ns/1'000'000'000,8);integer(ns%1'000'000'000,4);}
    void blob(const Bytes& v){integer(v.size(),8);bytes.insert(bytes.end(),v.begin(),v.end());}
    void string(const std::string& s){integer(s.size(),8);bytes.insert(bytes.end(),s.begin(),s.end());}
private:
    bool modern_;
};
class Reader {
public:
    Reader(const std::uint8_t* p,std::size_t size,bool modern=false):p_(p),left_(size),modern_(modern){}
    explicit Reader(const Bytes& b,bool modern=false):Reader(b.data(),b.size(),modern){}
    std::uint64_t integer(unsigned n){
        require(n>0&&n<=8,"surfxr_invalid_integer_width");
        if(!modern_||n==1)return raw_integer(n);
        const auto tag=raw_integer(1);if(tag<251)return tag;
        const unsigned width=tag==251?2:tag==252?4:tag==253?8:0;
        require(width&&width<=n,"surfxr_invalid_varint");return raw_integer(width);
    }
    bool boolean(){const auto v=integer(1);require(v<=1,"surfxr_invalid_bool");return v!=0;}
    float real(){const auto bits=static_cast<std::uint32_t>(raw_integer(4));float v;std::memcpy(&v,&bits,4);require(std::isfinite(v),"surfxr_nonfinite_float");return v;}
    std::uint64_t duration(){const auto seconds=integer(8),ns=integer(4);require(ns<1'000'000'000&&seconds<=(UINT64_MAX-ns)/1'000'000'000,"surfxr_invalid_duration");return seconds*1'000'000'000+ns;}
    std::string string(std::size_t limit=1024*1024){const auto n=integer(8);require(n<=limit&&n<=left_,"surfxr_invalid_length");std::string s(reinterpret_cast<const char*>(p_),static_cast<std::size_t>(n));p_+=n;left_-=n;return s;}
    Bytes blob(std::size_t limit=1024*1024){const auto n=integer(8);require(n<=limit&&n<=left_,"surfxr_invalid_length");Bytes b(p_,p_+n);p_+=n;left_-=n;return b;}
    void skip(std::size_t n){require_length(n);p_+=n;left_-=n;}
    void end() const{require(left_==0,"surfxr_trailing_bytes");}
    const std::uint8_t* data()const{return p_;}
    std::size_t remaining()const{return left_;}
private:
    void require_length(std::size_t n)const{require(n<=left_,"surfxr_truncated_packet");}
    std::uint64_t raw_integer(unsigned n){require(n<=8&&left_>=n,"surfxr_truncated_packet");std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p_[i])<<(8*i);p_+=n;left_-=n;return v;}
    const std::uint8_t* p_;std::size_t left_;bool modern_;
};
}
