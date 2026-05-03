

#pragma once

namespace base{

class UringProactorPool{
public:
    UringProactorPool(uint32_t size) : proactors_(size) {}

    void loop() {

        DispatchBrief([this](UringProactor* p){
            std::thread = [p](){
                p->loop();
            };
            
        });
    }

    void stop() {
        DispatchBrief([this](UringProactor* p){
            p->stop();
        });
    }

    size_t size() const { return proactors_.size(); }

    template <typename Func> 
    void DispatchBrief(Func&& f){
        for (unsigned i = 0; i < size(); ++i) {
            UringProactor& p = proactor_[i];

            p.DispatchBrief([p, func]() mutable { func(&p); });
        }        
    }    

    


private:
    std::vector<UringProactor> proactors_;
};


}