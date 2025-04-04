#pragma once

#include <atomic>
#include <cassert>
#include <vector>


#ifndef DISABLE_HAZARD

#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

#ifndef HAZARD_MAX
#define HAZARD_MAX 256  //max hazard pointers that a thread can have
#endif

template<typename T>
class HazardPointers {
public:
    static const int MAX_THREADS        = HAZARD_MAX; 
private:
    static const int MAX_HP_PER_THREAD  = 11;
    static const int CLPAD     = CACHE_LINE / sizeof(std::atomic<T*>);
    static const int THRESHOLD_R        = 0;
    static const int MAX_RETIRED        = MAX_THREADS * MAX_HP_PER_THREAD;

    const int maxHPs;
    const int maxThreads;

    //moltiplica per CACHE_LINE (molte celle vuote) ma no false sharing
    std::atomic<T*> Hazard [MAX_THREADS * CLPAD][MAX_HP_PER_THREAD];
    std::vector<T*> Retired[MAX_THREADS * CLPAD];

public:
    /**
     * @brief Constructor for hazard pointers matrix
     * 
     * The constructor initializes the hazard pointers matrix with `nullptr`
     * 
     * @param maxHPs (uint) maximum number of hazard pointers per thread
     * @param maxThreads (uint) maximum number of threads
     * 
     * 
     * @note The constructor asserts that maxHPs is less than or equal to `MAX_HP_PER_THREAD` and maxThreads is less than or equal to `MAX_THREADS`
     */
    HazardPointers(int maxHPs=MAX_HP_PER_THREAD, int maxThreads=MAX_THREADS):
    maxHPs{maxHPs}, maxThreads{maxThreads}
    {
        assert(maxHPs <= MAX_HP_PER_THREAD);
        assert(maxThreads <= MAX_THREADS);

        /**
         * Initialize only the cells that will be used by the threads
         */
        for(int iThread = 0; iThread < maxThreads; iThread++){
            for(int iHP = 0; iHP < maxHPs; iHP++){
                Hazard[iThread*CLPAD][iHP].store(nullptr,std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Destructor for hazard pointers matrix
     * 
     * Deallocates all pointers in the retired list
     */
    ~HazardPointers() 
    {
        for(int iThread = 0; iThread < MAX_THREADS; iThread++){
            for(unsigned iRet = 0; iRet < Retired[iThread*CLPAD].size(); iRet++){
                delete Retired[iThread * CLPAD][iRet];
            }
        }
    }

    
    /**
     * @brief Clear all hazard pointers for a given thread
     * @param tid (int) thread id
     */
    __attribute__((used,always_inline)) void clear(const int tid){
        for(int iHP = 0; iHP < maxHPs; iHP++){
            Hazard[tid * CLPAD][iHP].store(nullptr,std::memory_order_release);
        }
    }

    /**
     * @brief Clear a specific hazard pointer for a given thread
     * @param iHP (int) index of the hazard pointer
     * @param tid (int) thread id
     */
    __attribute__((used,always_inline)) void clear(const int iHP, const int tid){
        Hazard[tid * CLPAD][iHP].store(nullptr,std::memory_order_release);
    }

    /**
     * @brief Protects a pointer with a hazard pointer
     * @param index (int) index of the hazard pointer
     * @param atom (std::atomic<T*>&) pointer to protect
     * @param tid (int) thread id
     * 
     * @warning If the tid exeeds the maximum number of thread (given to constructor) the function doesn't protect the pointer
     * @return the value of the pointer
     * 
     */
    __attribute__((used,always_inline)) T* protect(const int index, const std::atomic<T*>& atom, const int tid){
        T* n = nullptr;
        T* ret;

        while((ret = atom.load()) != n){
            Hazard[tid * CLPAD][index].store(ret);
            n = ret;
        }
        return ret;
    }

    /**
     * @brief Protects a pointer with a hazard pointer
     * @param index (int) index of the hazard pointer
     * @param ptr (T*) pointer to protect
     * @param tid (int) thread id
     * 
     * @return the value of the pointer
     * @note This function is used when the pointer is not an atomic
     */
    __attribute__((used,always_inline)) T* protect(const int index, T* ptr, const int tid){
        Hazard[tid * CLPAD][index].store(ptr);
        return ptr;
    }

    /**
     * @brief Protects a pointer with a hazard pointer (memory order release)
     * @param index (int) index of the hazard pointer
     * @param ptr (T*) pointer to protect
     * @param tid (int) thread id
     * 
     * @return the value of the pointer
     * @note This function is used when the pointer is not an atomic
     */
    __attribute__((used,always_inline)) T* protectRelease(const int index, T* ptr, const int tid){
        Hazard[tid*CLPAD][index].store(ptr,std::memory_order_release);
        return ptr;
    }

    /**
     * @brief Retires a pointer
     * @param ptr (T*) pointer to retire
     * @param tid (int) thread id
     * 
     * Tries to delete the memory associated to any pointer present in the 
     * retired list. if no other thread is currently using it
     * Else it adds the pointer to the retired list.
     * 
     * @note If the number of retired pointers is greater than `THRESHOLD_R`, the function checks if the pointer can be deleted
     * 
     * @note if the pointer is nullptr tries to deallocate all the retired list
     */
    __attribute__((used,always_inline)) void retire(T* ptr, const int tid,bool check_thresh = false) 
    {
        if(ptr != nullptr) Retired[tid*CLPAD].push_back(ptr);
        if(check_thresh && Retired[tid*CLPAD].size() < THRESHOLD_R){
            return;
        }
        for(unsigned iRet = 0; iRet < Retired[tid * CLPAD].size();){
            auto obj = Retired[tid*CLPAD][iRet];

            bool canDelete = true;

            //check matrix to see if the object is still in use
            for(int tid = 0; tid < maxThreads && canDelete; tid++){
                for(int iHP = maxHPs -1; iHP >= 0; iHP--){
                    if(Hazard[tid*CLPAD][iHP].load() == obj){
                        canDelete = false;
                        break;
                    }
                }
            }
            if(canDelete){
                Retired[tid*CLPAD].erase(Retired[tid*CLPAD].begin() + iRet);
                delete obj;
                continue;
            }
            iRet++;
        }
        return;
    }

};


#else //stubs

template<typename T>
class HazardPointers {
public:
    HazardPointers( [[maybe_unused]] int maxHPs=0,
                    [[maybe_unused]] int maxThreads = 0) {}

    void clear(const int tid){}
    void clear(const int iHp, const int tid){}
    T* protect(const int iHp, std::atomic<T*>& atom,const int tid){return atom.load();}
    T* protect(const int iHp, T* ptr, const int tid){return ptr;}
    T* protectRelease(const int iHp, T* ptr, const int tid){return ptr;}
    void retire(T*, const int tid, bool check_thresh = true){}; //updated the stub to always return 0
};

#endif //_HAZARD_POINTERS_H_