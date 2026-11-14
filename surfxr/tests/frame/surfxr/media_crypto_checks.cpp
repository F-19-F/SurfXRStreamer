#include "surfxr/surfxr_unified_media.hpp"
#include <cstring>

namespace surfxr::transport {
void unified_media_crypto_checks(){
    desktop::Key key{},other{};key[0]=17;other[0]=18;
    const Bytes shard(1400,37);Bytes record,scratch{91},reused;
    unified::MediaCipher sender(key),receiver(key),wrong(other);
    sender.protection(unified::Protection::Plain);receiver.protection(unified::Protection::Plain);wrong.protection(unified::Protection::Plain);
    sender.seal(3,shard.data(),shard.size(),record);
    require(record.size()==shard.size()+20,"unified_plain_overhead");
    bool rejected=false;try{wrong.open(record.data(),record.size(),scratch);}catch(...){rejected=true;}
    require(rejected,"unified_plain_wrong_session_accepted");
    const auto view=receiver.open(record.data(),record.size(),scratch);
    Reader r(view.data,view.size);require(r.integer(4)==3&&r.remaining()==shard.size()&&
        view.data==record.data()+16&&scratch==Bytes{91}&&std::memcmp(r.data(),shard.data(),shard.size())==0,"unified_plain_borrow_failed");
    rejected=false;try{receiver.open(record.data(),record.size(),scratch);}catch(...){rejected=true;}
    require(rejected,"unified_plain_replay_accepted");
    rejected=false;try{sender.protection(unified::Protection::AesGcm);}catch(...){rejected=true;}
    require(rejected,"unified_media_mode_changed_after_packet");
    sender.seal(4,shard.data(),shard.size(),record);sender.seal(5,shard.data(),shard.size(),reused);
    receiver.open(reused.data(),reused.size(),scratch);receiver.open(record.data(),record.size(),scratch);
    std::array<std::uint8_t,20> prefix{};sender.plain_prefix(6,prefix);
    record.assign(prefix.begin(),prefix.end());record.insert(record.end(),shard.begin(),shard.end());
    const auto gathered=receiver.open(record.data(),record.size(),scratch);Reader gr(gathered.data,gathered.size);
    require(gr.integer(4)==6&&gr.remaining()==1400,"unified_plain_gather_prefix");
    unified::MediaCipher encrypted_sender(key),encrypted_receiver(key);desktop::Cipher legacy(key);
    encrypted_sender.seal(7,shard.data(),shard.size(),record);
    require(record==legacy.seal(unified::datagram(7,shard.data(),shard.size()))&&record.size()==shard.size()+28,"unified_aes_legacy_wire_changed");
    auto damaged=record;damaged.at(damaged.size()-1)^=1;rejected=false;
    try{encrypted_receiver.open(damaged.data(),damaged.size(),scratch);}catch(...){rejected=true;}
    require(rejected,"unified_aes_bad_tag_accepted");
    const auto decrypted=encrypted_receiver.open(record.data(),record.size(),scratch);Reader er(decrypted.data,decrypted.size);
    require(er.integer(4)==7&&er.remaining()==1400&&decrypted.data==scratch.data(),"unified_aes_auth_failure_consumed_sequence");
    for(unsigned i=0;i<32;++i){encrypted_sender.seal(9,shard.data(),shard.size(),record);encrypted_receiver.open(record.data(),record.size(),scratch);}
    const auto* data=record.data();const auto capacity=record.capacity();encrypted_sender.seal(10,shard.data(),shard.size(),record);
    require(data==record.data()&&capacity==record.capacity(),"unified_aes_record_buffer_not_reused");
    rejected=false;try{receiver.open(record.data(),record.size(),scratch);}catch(...){rejected=true;}
    require(rejected,"unified_plain_aes_mode_confusion");
}
}
