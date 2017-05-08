#include <assert.h>
#include <pthread.h>
#include "MOSI.h"
#include "Cache.h"
#include "Bus.h"
#include "Memory.h"
#include "Protocol.h"
#include <iostream>

MOSI::MOSI(int cache_id) {
    id = cache_id;
    pthread_t tid;
    pthread_create(&tid, NULL, request_worker, (void *) this);
    pthread_create(&tid, NULL, response_worker, (void *) this);
}

void *MOSI::response_worker(void *arg) {
    MOSI *obj = (MOSI *) arg;
    pthread_mutex_lock(&Bus::resp_lock);
    while(true) {
        while(!Bus::pending_work[obj->id]) {
            pthread_cond_wait(&Bus::resp_cvar, &Bus::resp_lock);
        }
        std::cout<<"Thread "<<obj->id<<" to respond\n";
        pthread_mutex_unlock(&Bus::resp_lock);

        pthread_mutex_lock(&obj->lock);


        if(obj->pending_addr == Bus::addr &&
                (obj->opt != BusRd || Bus::opt != BusRd)) {
            assert(obj->pending_addr);
            Bus::recv_nak = true;
        } else {
            char status = obj->cache.cache_check_status(Bus::addr);
            switch(status) {
                case 'M':
                    if(Bus::opt == BusRd) {
                        obj->cache.cache_set_status(Bus::addr, 'O');
                    } else {
                        /* No memory writeback */
                        assert(Bus::opt == BusRdX);
                        obj->cache.cache_set_status(Bus::addr, 'I');
                        Bus::owner_id = obj->id;
                    }
                    break;
                case 'O':
                    if(Bus::opt == BusRd || Bus::opt == BusRdX) {
                        Bus::owner_id = obj->id;
                    }
                    if(Bus::opt == BusRdX || Bus::opt == BusUpg) {
                        obj->cache.cache_set_status(Bus::addr, 'I');
                    }
                case 'S':
                    if(Bus::opt == BusRdX || Bus::opt == BusUpg) {
                        obj->cache.cache_set_status(Bus::addr, 'I');
                    }
                    break;
                case 'I':
                    break;
                default:
                    assert(0);
            }
        }

       pthread_mutex_unlock(&obj->lock);

        pthread_mutex_lock(&Bus::resp_lock);
        Bus::pending_work[obj->id] = false;
        Bus::resp_count++;
        if(Bus::resp_count == Protocol::num_cores - 1) {
            pthread_cond_signal(&Bus::req_cvar);
        }
        std::cout<<"Thread "<<obj->id<<" done responding\n";
    }
}

void *MOSI::request_worker(void *arg) {
    MOSI *obj = (MOSI *)arg;
    std::string op;
    unsigned long addr;

    pthread_mutex_lock(&Protocol::lock);
    while(true) {
        //fflush(stdout);
        while(!Protocol::ready || Protocol::request_id != obj->id) {
            pthread_cond_wait(&Protocol::worker_cv, &Protocol::lock);
        }
        //fflush(stdout);
        std::cout<<"Thread "<<obj->id<< " got request\n";
        op = Protocol::request_op;
        addr = Protocol::request_addr;

        Protocol::ready = false;
        pthread_cond_signal(&Protocol::trace_cv);
        pthread_mutex_unlock(&Protocol::lock);

        /* Lock is released before actually handling the request. This is
         * so that mem access processor can continue processing requests
         */
        handle_request(obj, op, addr);
        std::cout<<"Thread " << obj->id<<" Done with request\n";
        pthread_mutex_lock(&Protocol::lock);
        //std::cout<<"done handling\n";
        fflush(stdout);
    }
    pthread_mutex_unlock(&Protocol::lock);
    return NULL;
}

void MOSI::handle_request(MOSI *obj, std::string op, unsigned long addr) {
    assert(addr);
    bool done = false;
    bool cache_transfer = false;

    pthread_mutex_lock(&obj->lock);
    char status = obj->cache.cache_check_status(addr);
    if (status == 'M' ||
            (status == 'S' && op == "R") ||
            (status == 'O' && op == "R")) {
        obj->cache.update_cache_lru(addr);
        pthread_mutex_unlock(&obj->lock);
        return;
    }
    pthread_mutex_unlock(&obj->lock);

    while(!done) {
        pthread_mutex_lock(&Bus::req_lock);
        pthread_mutex_lock(&obj->lock);
        status = obj->cache.cache_check_status(addr);
        done = true;

        Protocol::bus_transactions++;

        switch(status) {
            case 'S':
            case 'O':
                assert(op == "W");
                obj->cache.update_cache_lru(addr);
                obj->opt = BusUpg;
                std::cout<<"Before wait\n";
                Bus::wait_for_responses(obj->id, addr, BusUpg);
                std::cout<<"After wait\n";
                if(Bus::recv_nak) {
                    done = false;
                } else {
                    obj->cache.cache_set_status(addr, 'M');
                }
                break;

            case 'I':
                std::cout<<"Cache miss\n";
                if(op == "R") {
                    obj->opt = BusRd;
                    std::cout<<"Before wait\n";
                    Bus::wait_for_responses(obj->id, addr, BusRd);
                    std::cout<<"After wait\n";
                    if(Bus::recv_nak) {
                        done = false;
                    } else {
                        obj->cache.insert_cache(addr, 'S');
                        obj->pending_addr = addr;
                    }
                } else {
                    obj->opt = BusRdX;
                    Bus::wait_for_responses(obj->id, addr, BusRdX);
                    if(Bus::recv_nak) {
                        done = false;
                    } else {
                        obj->cache.insert_cache(addr, 'M');
                        obj->pending_addr = addr;
                    }
                }
                break;
            default:
                assert(0);
        }
        /* Cache transfer will only matter if it was in 'I' state */
        if(Bus::owner_id != -1) {
            cache_transfer = true;
        }
        pthread_mutex_unlock(&obj->lock);
        pthread_mutex_unlock(&Bus::req_lock);


        if(status == 'I' && done) {
            if(cache_transfer) {
                Protocol::cache_transfers++;
            } else {
                Memory::request(addr);
                pthread_mutex_lock(&obj->lock);
                obj->pending_addr = 0;
                pthread_mutex_unlock(&obj->lock);
            }
            //std::cout<<"Done with memory request\n";
        }
   }
}