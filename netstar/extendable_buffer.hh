#ifndef _EXTENDABLE_BUFFER
#define _EXTENDABLE_BUFFER

#include "temporary_buffer.hh"

using namespace seastar;

class extendable_buffer{
    temporary_buffer<char> _buffer;
    size_t _data_len;
public:
    explicit extendable_buffer(size_t initial_buf_size) :
        _buffer(initial_buf_size),
        _data_len(0) {}

    extendable_buffer(const extendable_buffer& other) = delete;
    extendable_buffer& operator=(const extendable_buffer& other) = delete;

    extendable_buffer(extendable_buffer&& other) :
        _buffer(std::move(other._buffer)),
        _data_len(other._data_len) {}
    extendable_buffer& operator=(extendable_buffer& other){
        if(this != &other){
            _buffer = std::move(other._buffer);
            _data_len = other._data_len;
        }
        return *this;
    }

    void clear_data(){
        _data_len = 0;
    }
    void fill_data(const char* src, size_t size){
        if(_buffer.size() < size){
            _buffer = temporary_buffer<char>(size);
        }
        std::copy_n(src, size, _buffer.get_write());
        _data_len = size;
    }

    size_t data_len(){
        return _data_len;
    }
    const char* data(){
        return _buffer.get();
    }
};

#endif
