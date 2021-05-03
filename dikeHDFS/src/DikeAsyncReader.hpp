#ifndef DIKE_ASYNC_READER_HPP
#define DIKE_ASYNC_READER_HPP

#include <iostream>
#include <chrono> 
#include <ctime>
#include <string>
#include <sstream> 
#include <iomanip>
#include <thread>
#include <queue>
#include <mutex>
#include <cassert>
#include <semaphore.h>
#include <unistd.h>

#include <string.h>

#include "DikeUtil.hpp"
#include "DikeBuffer.hpp"
#include "DikeIO.hpp"

class DikeRecord {
    public:
    enum{
        FIELED_SIZE  = 1024,
        MAX_COLUMNS = 128
    };
    int nCol;
    uint8_t * fields[MAX_COLUMNS];
    uint8_t * fieldMemory[MAX_COLUMNS];
    int len[MAX_COLUMNS];    

    DikeRecord(int col) {
        nCol = col;
        uint8_t * buf = (uint8_t *)malloc(FIELED_SIZE * MAX_COLUMNS);
        for(int i = 0; i < nCol; i++) {
            fields[i] = 0;
            fieldMemory[i] = buf + i * FIELED_SIZE;
            len[i] = 0;            
        }
    }
    ~DikeRecord(){
        free(fieldMemory[0]);
    }
};

class DikeAsyncReader{
    public:
    enum{
        QUEUE_SIZE  = 4,
        BUFFER_SIZE = (128 << 10)
    };

    DikeIO * input = NULL;

    uint64_t blockSize = 0; /* Limit reads to one HDFS block */
    uint64_t blockOffset = 0; /* If not zero we need to seek for record */   
    uint8_t * memPool = 0;
    int bytesRead = 0; /* How many bytes did we read so far */
    int fDelim = ','; /* Field delimiter */
    int rDelim = '\n'; /* Record Delimiter */
    int qDelim = '\"'; /* Quotation Delimiter */

    DikeRecord * record = NULL; /* Single record */

    std::queue<DikeBuffer * > work_q;
    std::queue<DikeBuffer * > free_q;
    std::queue<DikeBuffer * > tmp_q;
    std::mutex q_lock;
    sem_t work_sem;
    sem_t free_sem;    
    std::thread workerThread;
    bool isRunning;
    DikeBuffer * buffer = NULL;
    int pushCount = 0;
    int emptyCount = 0;
    uint64_t recordCount = 0;

    DikeAsyncReader(DikeIO * input, uint64_t blockSize){
        this->input = input;        
        this->blockSize = blockSize;
        
        memPool = (uint8_t *)malloc(QUEUE_SIZE * BUFFER_SIZE);
        for(int i = 0; i < QUEUE_SIZE; i++){
            DikeBuffer * b = new DikeBuffer(&memPool[i * BUFFER_SIZE], BUFFER_SIZE);
            b->id = i;
            free_q.push(b);
        }

        sem_init(&work_sem, 0, 0);
        sem_init(&free_sem, 0, QUEUE_SIZE);
               
        isRunning = true;
        workerThread = startWorker(); // This will start reading immediatelly
        buffer = getBuffer();
    }

    ~DikeAsyncReader(){
        isRunning = false;
        //std::cout << "~DikeAsyncReader" << std::endl;
        sem_post(&free_sem);
        workerThread.join();
        sem_destroy(&work_sem);
        sem_destroy(&free_sem);

        delete buffer;
        DikeBuffer * b;
        while(!free_q.empty()){
            b = free_q.front();
            free_q.pop();
            delete b;
        }
        while(!work_q.empty()){
            b = work_q.front();
            work_q.pop();
            delete b;
        }
        while(!tmp_q.empty()){
            b = tmp_q.front();
            tmp_q.pop();
            delete b;
        }        
        if(record){
            delete record;
        }
        if(memPool){
            free(memPool);
        }
        //std::cout << "~DikeAsyncReader Push count: " << pushCount << " Empty count: " << emptyCount << " bytesRead " << bytesRead << std::endl;        
    }    

    int initRecord(int nCol){
        record = new DikeRecord(nCol);
        return (record != NULL);
    }

    bool isEOF() {
        if(blockSize > 0 && bytesRead > blockSize) {
            return true;
        }
        return false;
    }
    
    int seekRecord() {
        uint8_t * posPtr = buffer->posPtr;        
        bool underQuote = false;
        while(posPtr < buffer->endPtr) {
            if(*posPtr == qDelim){
                underQuote = !underQuote;
            }
            if(!underQuote){
                if(*posPtr == rDelim){
                    bytesRead += posPtr - buffer->posPtr + 1;
                    buffer->posPtr = posPtr + 1; // Skiping delimiter            
                    return 0;
                }
            }
            posPtr++;
        }        

        std::cout << "DikeAsyncReader failed seek " << std::endl;
        return 1;
    }
    
    int getColumnCount(){
        int nCol = 0;
        uint8_t * posPtr = buffer->posPtr;
        bool underQuote = false;

        while(posPtr < buffer->endPtr) {
            if(*posPtr == qDelim){
                underQuote = !underQuote;
            }
            if(!underQuote){
                if(*posPtr == fDelim){
                    nCol++;
                } else if (*posPtr == rDelim) {
                    nCol++;
                    break;
                }
            }
            posPtr++;
        }

        std::cout << "DikeAsyncReader detected " << nCol << " columns" << std::endl;

        return nCol;
    }

    int readRecord() {
        if(isEOF()){
            //std::cout << "DikeAsyncReader EOF at " << bytesRead << std::endl;
            return 1;
        }
        releaseBuffers();

        recordCount++;
        for(int i = 0; i < record->nCol; i++) {
            record->fields[i] = NULL;
            if(readField(i)){
                //std::cout << "DikeAsyncReader EOF at " << bytesRead << std::endl;
                return 1;
            }
        }

        return 0;
    }    

    int readField(int pos) {
        uint8_t * posPtr = buffer->posPtr;
        DikeBuffer * orig_buffer = buffer;
        bool underQuote = false;
        record->fields[pos] = buffer->posPtr;

        while(posPtr < buffer->endPtr) {
            if(*posPtr == qDelim){
                underQuote = !underQuote;
            }
            if(!underQuote){
                if(*posPtr == fDelim || *posPtr == rDelim){                                                      
                    record->len[pos] = posPtr - record->fields[pos] + 1;
                    *posPtr = 0; // SQLite expects strings to be NULL terminated
                    posPtr++;
                    bytesRead += posPtr - buffer->posPtr;
                    buffer->posPtr = posPtr;
                    return 0;
                }
            }
            posPtr++;
        }

        // Use internal memory
        int count = 0;
        uint8_t * fieldPtr = record->fieldMemory[pos];
        record->fields[pos] = fieldPtr;
        // Copy first part
        posPtr = buffer->posPtr;
        while( posPtr < buffer->endPtr) {
            *fieldPtr = *posPtr;
            posPtr++;
            fieldPtr++;
            count++;
        }
        // Get new buffer
        holdBuffer(buffer);
        buffer = getBuffer();
        posPtr = buffer->posPtr;
        // Copy second part
        while(posPtr < buffer->endPtr) {            
            if(*posPtr == qDelim){
                underQuote = !underQuote;
            }
            if(!underQuote){
                if(*posPtr == fDelim || *posPtr == rDelim){
                    count++;
                    *fieldPtr = 0; // SQLite expects strings to be NULL terminated
                    posPtr++;
                    fieldPtr++;

                    record->len[pos] = count;                    
                    bytesRead += count;
                    buffer->posPtr = posPtr;
                    return 0;
                }
            }
            count++;
            *fieldPtr = *posPtr;
            posPtr++;
            fieldPtr++;
        }
        
        //std::cout << "DikeAsyncReader End of data 2 at  " << bytesRead << std::endl;
        return 1;
    }

    void holdBuffer(DikeBuffer * buf) {
        //q_lock.lock();       
        tmp_q.push(buf);        
        //q_lock.unlock();        
    }

    void releaseBuffers(void) {
        DikeBuffer * b;
        //q_lock.lock();       
        while(!tmp_q.empty()){
            b = tmp_q.front();
            tmp_q.pop();
            pushBuffer(b);
        }
        //q_lock.unlock();        
    }

    void pushBuffer(DikeBuffer * buf) {        
        q_lock.lock();
        pushCount ++; // We processed this buffer
        if(free_q.empty()){
            emptyCount++; // We reading faster than processing
        }
        free_q.push(buf);        
        q_lock.unlock();
        sem_post(&free_sem);
    }

    DikeBuffer * getBuffer(){
        sem_wait(&work_sem);
        q_lock.lock();
        DikeBuffer * b = work_q.front();
        work_q.pop();
        q_lock.unlock();
        //std::cout << "DikeAsyncReader buffer " << b->id << " retrieved " << std::endl;
        return b;
    }

    std::thread startWorker() {
        return std::thread([=] { Worker(); });
    }

    void Worker() {
        pthread_t thread_id = pthread_self();
        pthread_setname_np(thread_id, "DikeAsyncReader::Worker");

        while(1){
            sem_wait(&free_sem);
            if(isEOF()){
                //std::cout << "DikeAsyncReader EOF exiting worker thread" << std::endl;
                return;                
            }
            if(!isRunning){
                //std::cout << "DikeAsyncReader not running is set exiting " << std::endl;
                return;
            }

            q_lock.lock();
            if(free_q.empty()){
                std::cout << "DikeAsyncReader exiting worker thread" << std::endl;
                q_lock.unlock();
                return;
            }
            DikeBuffer * b = free_q.front();             
            free_q.pop();
            q_lock.unlock();

            b->reset();

            int n = input->read((char *)b->startPtr, BUFFER_SIZE);
            b->setReadableBytes(n);            
            if(n < BUFFER_SIZE && n > 0){
                memset((char*)&b->startPtr[n], 0, BUFFER_SIZE - n);
            }
            //std::cout << "DikeAsyncReader buffer " << b->id << " ready " << std::endl;
            q_lock.lock();
            work_q.push(b);            
            q_lock.unlock();
            sem_post(&work_sem);
        }
    }
};

#endif /* DIKE_ASYNC_READER_HPP */