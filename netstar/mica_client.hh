#ifndef _MICA_CLIENT
#define _MICA_CLIENT

#include "netstar/port.hh"
#include "netstar/work_unit.hh"

namespace netstar {

class mica_client : public work_unit<mica_client>{
public:
    explicit mica_client(per_core_objs<mica_client>* all_objs) :
        work_unit<mica_client>(all_objs) {}

    mica_client(const mica_client& other) = delete;
    mica_client(mica_client&& other)  = delete;
    mica_client& operator=(const mica_client& other) = delete;
    mica_client& operator=(mica_client&& other) = delete;


};

} // namespace netstar

#endif // _MICA_CLIENT
