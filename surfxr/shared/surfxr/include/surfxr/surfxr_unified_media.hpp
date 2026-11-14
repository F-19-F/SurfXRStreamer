#pragma once
#include "surfxr/surfxr_desktop_crypto.hpp"
#include "surfxr/surfxr_unified.hpp"
#include <array>
#include <cstring>

namespace surfxr::transport::unified {
enum class Protection:unsigned {Plain=0,AesGcm=1};
constexpr unsigned media_negotiation_flag=4,media_aes_flag=8;
struct MediaView {const std::uint8_t* data=nullptr;std::size_t size=0;};

// Authentication/control select one mode before any media is sent. Old peers
// retain the original AES record. Plain records have session/channel isolation
// and duplicate rejection, but provide no media confidentiality or integrity.
class MediaCipher {
    desktop::Cipher aes_;
    desktop::ReplayWindow replay_;
    Bytes aes_plain_;
    std::uint64_t cookie_=0,sent_=0;
    Protection protection_=Protection::AesGcm;
    bool used_=false;
    static void integer(std::uint8_t* out,std::uint64_t value,unsigned size){
        for(unsigned i=0;i<size;++i)out[i]=static_cast<std::uint8_t>(value>>(8*i));
    }
public:
    explicit MediaCipher(const desktop::Key& key):aes_(key){
        constexpr char domain[]="ALVR-UNIFIED-plain-cookie-v1";
        desktop::Hmac mac(key);const auto digest=mac.digest(reinterpret_cast<const std::uint8_t*>(domain),sizeof(domain)-1);
        for(unsigned i=0;i<8;++i)cookie_|=std::uint64_t(digest[i])<<(8*i);
    }
    void protection(Protection value){
        require(!used_&&(value==Protection::Plain||value==Protection::AesGcm),"surfxr_unified_media_mode_locked");protection_=value;
    }
    Protection protection()const{return protection_;}
    unsigned overhead()const{return protection_==Protection::Plain?20:28;}
    void plain_prefix(std::uint32_t generation,std::array<std::uint8_t,20>& prefix){
        require(protection_==Protection::Plain&&sent_!=UINT64_MAX,"surfxr_unified_media_sequence_exhausted");used_=true;
        integer(prefix.data(),cookie_,8);integer(prefix.data()+8,++sent_,8);integer(prefix.data()+16,generation,4);
    }
    void seal(std::uint32_t generation,const std::uint8_t* shard,std::size_t size,Bytes& output){
        require(shard&&size>=14&&size<=2048,"surfxr_unified_datagram_bound");
        if(protection_==Protection::Plain){
            std::array<std::uint8_t,20> prefix{};plain_prefix(generation,prefix);output.resize(prefix.size()+size);
            std::memcpy(output.data(),prefix.data(),prefix.size());std::memcpy(output.data()+prefix.size(),shard,size);
        }else{
            used_=true;aes_plain_.resize(4+size);integer(aes_plain_.data(),generation,4);
            std::memcpy(aes_plain_.data()+4,shard,size);aes_.seal_into(aes_plain_,output);
        }
    }
    MediaView open(const std::uint8_t* record,std::size_t size,Bytes& scratch){
        require(record&&size>=overhead()+14&&size<=overhead()+2048,"surfxr_unified_datagram_bound");used_=true;
        if(protection_==Protection::AesGcm){aes_.open_into(record,size,scratch,false);return {scratch.data(),scratch.size()};}
        Reader r(record,size);const auto cookie=r.integer(8),sequence=r.integer(8);
        require(cookie==cookie_,"surfxr_unified_media_session");
        require(replay_.accepts(sequence),"desktop_media_replay");replay_.commit(sequence);
        return {record+16,size-16};
    }
};
}
