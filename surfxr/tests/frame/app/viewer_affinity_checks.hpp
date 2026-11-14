#include <fstream>
#pragma once
#include "surfxr/cpu_affinity.hpp"
#include <stdexcept>
namespace surfxr::tests {

inline void require(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
inline std::string read(const std::string& path){std::ifstream file(path);return std::string(std::istreambuf_iterator<char>(file),{});}

inline void viewer_affinity_checks(){
const auto require=[](bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);};
        {
            unsigned capacity=17;
            require(surfxr::parse_cpu_capacity(" 1024\n",capacity)&&capacity==1024,"cpu_capacity_not_parsed");
            for(const auto text:{""," ","0","-1","+1","1 2","1024tail","999999999999999999999999999999999999"})
                require(!surfxr::parse_cpu_capacity(text,capacity)&&capacity==1024,"invalid_cpu_capacity_changed_output");
            cpu_set_t allowed,selected;CPU_ZERO(&allowed);CPU_ZERO(&selected);
            CPU_SET(0,&allowed);CPU_SET(2,&allowed);CPU_SET(4,&allowed);
            std::array<unsigned,CPU_SETSIZE> capacities{};
            capacities[0]=216;capacities[2]=capacities[4]=855;capacities[7]=1024;
            require(surfxr::select_render_cpu_mask(allowed,capacities,selected)&&surfxr::affinity_count(selected)==2
                    &&CPU_ISSET(2,&selected)&&CPU_ISSET(4,&selected)&&!CPU_ISSET(7,&selected),"render_capacity_must_respect_allowed_mask");
            capacities[4]=1024;
            require(surfxr::select_render_cpu_mask(allowed,capacities,selected)&&surfxr::affinity_count(selected)==1
                    &&CPU_ISSET(4,&selected),"render_capacity_strongest_cpu_not_selected");
            const auto previous=selected;
            capacities[2]=0;
            require(!surfxr::select_render_cpu_mask(allowed,capacities,selected)&&CPU_EQUAL(&selected,&previous),"unknown_capacity_changed_mask");
            capacities[0]=capacities[2]=capacities[4]=1024;
            require(!surfxr::select_render_cpu_mask(allowed,capacities,selected)&&CPU_EQUAL(&selected,&previous),"homogeneous_capacity_changed_mask");
            CPU_ZERO(&allowed);
            require(!surfxr::select_render_cpu_mask(allowed,capacities,selected)&&CPU_EQUAL(&selected,&previous),"empty_capacity_mask_changed_output");
            cpu_set_t before,after;
            require(sched_getaffinity(0,sizeof(before),&before)==0,"render_inherit_mask_unavailable");
            const auto inherited=surfxr::configure_render_cpu_affinity(false);
            require(!inherited.error&&inherited.effective_known&&!inherited.attempted&&!inherited.applied
                    &&sched_getaffinity(0,sizeof(after),&after)==0&&CPU_EQUAL(&before,&after),"render_inherit_changed_affinity");
        }
}
}
