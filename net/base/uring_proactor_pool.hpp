

#pragma once
#include "uring_proactor.hpp"
#include "util/thread.hpp"

#include <memory>
#include <vector>

namespace base{

class UringProactorPool{
public:
    UringProactorPool(uint32_t size) : proactors_(size) {
        for(auto i = 0;i < proactors_.size(); ++i){
            proactors_[i] = std::make_unique<UringProactor>();
        }


        for(auto i = 0;i < proactors_.size(); ++i){
            threads.emplace_back(std::make_unique<util::Thread>());
        }
    }

    void AsyncLoop() {

        for(auto i = 0;i < proactors_.size(); ++i){
            threads[i] = util::Thread([this]{
                proactors[i] ->loop();
            })            
        }
    }

    void stop() {

        
        DispatchBrief([this](UringProactor* p){
            p->stop();
        });

        for(auto i = 0;i < proactors_.size(); ++i){
            threads[i].join();           
        }        
    }

    size_t size() const { return proactors_.size(); }

    template <typename Func> 
    void DispatchBrief(Func&& f){
        for (unsigned i = 0; i < size(); ++i) {
            auto& p = proactor_[i];

            p->DispatchBrief([p, func]() mutable { func(&p); });
        }        
    }    


    auto& at(size_t index) const { return proactors_[index]; }

    auto& operator[](size_t index) const { return at(index); }

private:
    std::vector<std::unique_ptr<UringProactor>> proactors_;
    std::vector<std::unique_ptr<util::Thread>> threads_;
};


}