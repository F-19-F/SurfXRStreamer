#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#endif

namespace surfxr::transport::crypto {
using Bytes=std::vector<std::uint8_t>;
using Key=std::array<std::uint8_t,32>;
class CachedHmac {
public:
    explicit CachedHmac(const Key& key):CachedHmac(key.data(),key.size()){}
    CachedHmac(const std::uint8_t* key,std::size_t size){
        if(!key||!size||size>65536)throw std::invalid_argument("surfxr_hmac_invalid_key");
        try{
#ifdef _WIN32
            checked(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG),"surfxr_hmac_provider_failed");
            DWORD bytes=0,returned=0;
            checked(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),sizeof(bytes),&returned,0),"surfxr_hmac_size_failed");
            if(returned!=sizeof(bytes)||!bytes)throw std::runtime_error("surfxr_hmac_object_size_invalid");
            object_.resize(bytes);
            checked(BCryptCreateHash(algorithm_,&hash_,object_.data(),bytes,const_cast<PUCHAR>(key),static_cast<ULONG>(size),BCRYPT_HASH_REUSABLE_FLAG),"surfxr_hmac_create_failed");
#else
            algorithm_=EVP_MAC_fetch(nullptr,"HMAC",nullptr);
            if(!algorithm_)throw std::runtime_error("surfxr_hmac_provider_failed");
            context_=EVP_MAC_CTX_new(algorithm_);
            if(!context_)throw std::runtime_error("surfxr_hmac_create_failed");
            char digest_name[]="SHA256";
            OSSL_PARAM params[]{OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,digest_name,0),OSSL_PARAM_construct_end()};
            checked(EVP_MAC_init(context_,key,size,params),"surfxr_hmac_init_failed");
#endif
        }catch(...){release();throw;}
    }
    ~CachedHmac(){release();}
    CachedHmac(const CachedHmac&)=delete;
    CachedHmac& operator=(const CachedHmac&)=delete;
    Key digest(const std::uint8_t* data,std::size_t size){
        if((!data&&size)||size>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("surfxr_hmac_invalid_input");
        if(failed_)throw std::runtime_error("surfxr_hmac_failed_state");
        Key result{};
        try{
#ifdef _WIN32
            if(size)checked(BCryptHashData(hash_,const_cast<PUCHAR>(data),static_cast<ULONG>(size),0),"surfxr_hmac_update_failed");
            checked(BCryptFinishHash(hash_,result.data(),static_cast<ULONG>(result.size()),0),"surfxr_hmac_final_failed");
#else
            checked(EVP_MAC_init(context_,nullptr,0,nullptr),"surfxr_hmac_reset_failed");
            if(size)checked(EVP_MAC_update(context_,data,size),"surfxr_hmac_update_failed");
            std::size_t written=0;
            checked(EVP_MAC_final(context_,result.data(),&written,result.size()),"surfxr_hmac_final_failed");
            if(written!=result.size())throw std::runtime_error("surfxr_hmac_result_size_invalid");
#endif
            return result;
        }catch(...){failed_=true;throw;}
    }
private:
    bool failed_=false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
    Bytes object_;
    static void checked(NTSTATUS result,const char* error){if(result<0)throw std::runtime_error(error);}
    void release()noexcept{
        if(hash_){BCryptDestroyHash(hash_);hash_=nullptr;}
        if(!object_.empty())SecureZeroMemory(object_.data(),object_.size());
        if(algorithm_){BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;}
    }
#else
    EVP_MAC* algorithm_=nullptr;
    EVP_MAC_CTX* context_=nullptr;
    static void checked(int result,const char* error){if(result!=1)throw std::runtime_error(error);}
    void release()noexcept{EVP_MAC_CTX_free(context_);context_=nullptr;EVP_MAC_free(algorithm_);algorithm_=nullptr;}
#endif
};
}
