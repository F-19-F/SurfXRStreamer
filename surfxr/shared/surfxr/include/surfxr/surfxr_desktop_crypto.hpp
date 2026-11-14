#pragma once
#include "surfxr/surfxr_codec.hpp"
#include "surfxr/surfxr_hmac.hpp"
#include <bitset>
#include <fstream>
#include <memory>
#ifndef _WIN32
#include <openssl/rand.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace surfxr::transport::desktop {
using Key=surfxr::transport::crypto::Key;
using Hmac=surfxr::transport::crypto::CachedHmac;
inline Key random(){
    Key out{};
#ifdef _WIN32
    require(BCryptGenRandom(nullptr,out.data(),32,BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"desktop_random_failed");
#else
    require(RAND_bytes(out.data(),32)==1,"desktop_random_failed");
#endif
    return out;
}
inline bool equal(const Key& a,const Key& b){unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=a[i]^b[i];return diff==0;}
inline Key proof(const Key& root,const Key& server,const Key& client,const std::string& domain){
    Bytes b(domain.begin(),domain.end());b.insert(b.end(),server.begin(),server.end());b.insert(b.end(),client.begin(),client.end());
    Hmac mac(root);return mac.digest(b.data(),b.size());
}
inline Key read_key(const std::string& path){
    std::string text;
#ifdef _WIN32
    std::ifstream f(path,std::ios::binary);require(bool(f),"desktop_key_unreadable");char b[66]{};f.read(b,sizeof(b));text.assign(b,static_cast<std::size_t>(f.gcount()));
#else
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"desktop_key_unreadable");
    struct stat st{};const bool valid=::fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&(st.st_mode&0077)==0&&st.st_size>=64&&st.st_size<=65;
    char b[66]{};const auto n=valid?::read(fd,b,sizeof(b)):-1;::close(fd);require(n>=64,"desktop_key_requires_private_regular_file");text.assign(b,static_cast<std::size_t>(n));
#endif
    if(text.size()==65&&text.back()=='\n')text.pop_back();
    require(text.size()==64,"desktop_key_invalid");Key key{};
    const auto hex=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;fail("desktop_key_invalid");};
    for(unsigned i=0;i<32;++i)key[i]=static_cast<std::uint8_t>((hex(text[2*i])<<4)|hex(text[2*i+1]));
    return key;
}
// Independent keys for each direction and channel; fresh challenge per TCP
// connection. AES-256-GCM encrypts media and input. Sequence is authenticated.
class ReplayWindow {
public:
    static constexpr std::size_t capacity=4096;
    bool accepts(std::uint64_t seq,bool ordered=false)const noexcept{
        if(!seq)return false;
        if(ordered)return received_!=UINT64_MAX&&seq==received_+1;
        return seq>received_||(received_-seq<capacity&&!seen_.test(slot(seq)));
    }
    // Commit only after validation/authentication succeeds. Circular positions
    // clear only newly exposed slots, rather than shifting every bit per packet.
    void commit(std::uint64_t seq)noexcept{
        if(seq>received_){
            const auto delta=seq-received_;
            if(delta>=capacity)seen_.reset();
            else for(std::uint64_t step=1;step<=delta;++step)seen_.reset(slot(received_+step));
            received_=seq;
        }
        seen_.set(slot(seq));
    }
    std::uint64_t latest()const noexcept{return received_;}
private:
    static std::size_t slot(std::uint64_t seq)noexcept{return static_cast<std::size_t>(seq&(capacity-1));}
    std::uint64_t received_=0;std::bitset<capacity> seen_;
};
class Cipher {
public:
    explicit Cipher(const Key& key){
#ifdef _WIN32
        try{
        require(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_AES_ALGORITHM,nullptr,0)>=0,"desktop_cipher_provider");
        require(BCryptSetProperty(algorithm_,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0)>=0,"desktop_cipher_mode");
        DWORD size=0,n=0;require(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&n,0)>=0,"desktop_cipher_size");object_.resize(size);
        require(BCryptGenerateSymmetricKey(algorithm_,&key_,object_.data(),size,const_cast<PUCHAR>(key.data()),32,0)>=0,"desktop_cipher_key");
        }catch(...){cleanup();throw;}
#else
        key_=key;
#endif
    }
    ~Cipher(){
#ifdef _WIN32
        cleanup();
#else
        EVP_CIPHER_CTX_free(seal_context_);EVP_CIPHER_CTX_free(open_context_);OPENSSL_cleanse(key_.data(),key_.size());
#endif
    }
    Cipher(const Cipher&)=delete;Cipher& operator=(const Cipher&)=delete;
    Bytes seal(const Bytes& plain){Bytes record;seal_into(plain,record);return record;}
    void seal_into(const Bytes& plain,Bytes& record){seal_into(plain.data(),plain.size(),record);}
    // Input and output must not alias. Capacity is retained between records;
    // ciphertext is written directly after its eight-byte sequence header.
    void seal_into(const std::uint8_t* plain,std::size_t size,Bytes& record){
        require(!aliases(plain,size,record),"desktop_cipher_buffer_alias");
        try{
            require((plain||!size)&&size<2*1024*1024,"desktop_cipher_record_size");
            require(sent_!=UINT64_MAX,"desktop_cipher_sequence_exhausted");
            const auto seq=++sent_;record.resize(size+24);
            for(unsigned i=0;i<8;++i)record[i]=static_cast<std::uint8_t>(seq>>(8*i));
            crypt(plain,size,record.data()+8,seq,true,record.data()+8+size);
        }catch(...){clear_output(record);throw;}
    }
    Bytes open(const Bytes& record,bool ordered=true){return open(record.data(),record.size(),ordered);}
    Bytes open(const std::uint8_t* record,std::size_t size,bool ordered=true){
        Bytes plain;open_into(record,size,plain,ordered);return plain;
    }
    void open_into(const Bytes& record,Bytes& plain,bool ordered=true){open_into(record.data(),record.size(),plain,ordered);}
    void open_into(const std::uint8_t* record,std::size_t size,Bytes& plain,bool ordered=true){
        require(!aliases(record,size,plain),"desktop_cipher_buffer_alias");
        try{
            require(record&&size>=24&&size<=2*1024*1024,"desktop_cipher_record_size");Reader r(record,size);const auto seq=r.integer(8);
            require(seq>0,"desktop_cipher_sequence");
            require(window_.accepts(seq,ordered),ordered?"desktop_control_sequence":"desktop_media_replay");
            const auto count=size-24;std::array<std::uint8_t,16> tag{};std::copy_n(record+size-16,16,tag.begin());
            plain.resize(count+16);crypt(record+8,count,plain.data(),seq,false,tag.data());
            plain.resize(count);window_.commit(seq);
        }catch(...){clear_output(plain);throw;}
    }
private:
    std::uint64_t sent_=0;ReplayWindow window_;
    static bool aliases(const std::uint8_t* input,std::size_t size,const Bytes& output)noexcept{
        if(!input||!size||!output.capacity())return false;
        const auto src=reinterpret_cast<std::uintptr_t>(input),dst=reinterpret_cast<std::uintptr_t>(output.data());
        return src>=dst?src-dst<output.capacity():dst-src<size;
    }
    static void clear_output(Bytes& output)noexcept{
        if(!output.empty()){
#ifdef _WIN32
            SecureZeroMemory(output.data(),output.size());
#else
            OPENSSL_cleanse(output.data(),output.size());
#endif
        }
        output.clear();
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm_=nullptr;BCRYPT_KEY_HANDLE key_=nullptr;Bytes object_;
    void cleanup()noexcept{if(key_)BCryptDestroyKey(key_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);if(!object_.empty())SecureZeroMemory(object_.data(),object_.size());key_=nullptr;algorithm_=nullptr;}
#else
    Key key_{};EVP_CIPHER_CTX* seal_context_=nullptr;EVP_CIPHER_CTX* open_context_=nullptr;
    EVP_CIPHER_CTX* context(bool encrypt){
        auto& owner=encrypt?seal_context_:open_context_;
        if(!owner){
            auto* fresh=EVP_CIPHER_CTX_new();require(fresh,"desktop_cipher_context");
            if(EVP_CipherInit_ex(fresh,EVP_aes_256_gcm(),nullptr,key_.data(),nullptr,encrypt?1:0)!=1){
                EVP_CIPHER_CTX_free(fresh);fail("desktop_cipher_init");
            }
            owner=fresh;
        }
        return owner;
    }
#endif
    void crypt(const std::uint8_t* input,std::size_t count,std::uint8_t* output,std::uint64_t seq,bool encrypt,std::uint8_t* tag){
        std::array<std::uint8_t,12> nonce{};for(unsigned i=0;i<8;++i)nonce[4+i]=static_cast<std::uint8_t>(seq>>(8*i));
#ifdef _WIN32
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);info.pbNonce=nonce.data();info.cbNonce=12;info.pbAuthData=nonce.data();info.cbAuthData=12;info.pbTag=tag;info.cbTag=16;
        ULONG written=0;const auto result=encrypt?BCryptEncrypt(key_,const_cast<PUCHAR>(input),static_cast<ULONG>(count),&info,nullptr,0,output,static_cast<ULONG>(count+16),&written,0):BCryptDecrypt(key_,const_cast<PUCHAR>(input),static_cast<ULONG>(count),&info,nullptr,0,output,static_cast<ULONG>(count+16),&written,0);
        require(result>=0&&written==count,"desktop_cipher_authentication");
#else
        auto* active=context(encrypt);
        // A fresh nonce resets GCM's per-record state while retaining the key
        // schedule/provider context. Direction owners never change operation.
        require(EVP_CipherInit_ex(active,nullptr,nullptr,nullptr,nonce.data(),-1)==1,"desktop_cipher_init");int written=0,tail=0;
        require(EVP_CipherUpdate(active,nullptr,&written,nonce.data(),12)==1,"desktop_cipher_aad");
        written=0;if(count)require(EVP_CipherUpdate(active,output,&written,input,static_cast<int>(count))==1,"desktop_cipher_update");
        if(!encrypt)require(EVP_CIPHER_CTX_ctrl(active,EVP_CTRL_GCM_SET_TAG,16,tag)==1,"desktop_cipher_tag");
        require(EVP_CipherFinal_ex(active,output+written,&tail)==1,"desktop_cipher_authentication");
        require(written==static_cast<int>(count)&&tail==0,"desktop_cipher_output_size");
        if(encrypt)require(EVP_CIPHER_CTX_ctrl(active,EVP_CTRL_GCM_GET_TAG,16,tag)==1,"desktop_cipher_tag");
#endif
    }
};
}
