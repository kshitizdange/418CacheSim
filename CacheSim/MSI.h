#include "Protocol.h"

#ifndef _MSI_H_
#define _MSI_H_

class MSI : public Protocol {
public:
    MSI() ;

    static void *request_worker(void *arg);

    static void handle_request(int tid);
};

#endif /* _MSI_H_ */