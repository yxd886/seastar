#ifndef _EXTENDABLE_BUFFER
#define _EXTENDABLE_BUFFER

#include "core/temporary_buffer.hh"
#include "net/packet.hh"
#include "roundup.hh"

using namespace seastar;



class extendable_buffer{
    temporary_buffer<char> _buffer;
    size_t _data_len;
public:
    explicit extendable_buffer(size_t initial_buf_size) :
        _buffer(initial_buf_size),
        _data_len(0) {}
    extendable_buffer() : _buffer(), _data_len(0) {}

    extendable_buffer(const extendable_buffer& other) = delete;
    extendable_buffer& operator=(const extendable_buffer& other) = delete;

    extendable_buffer(extendable_buffer&& other) :
        _buffer(std::move(other._buffer)),
        _data_len(other._data_len) {
        other._data_len = 0;
    }
    extendable_buffer& operator=(extendable_buffer&& other){
        if(this != &other){
            _buffer = std::move(other._buffer);
            _data_len = other._data_len;
            other._data_len = 0;
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
    template<typename T>
    void fill_data(T& obj){
        static_assert(std::is_pod<T>::value, "The provided object is not a plain-old-datatype.\n");
        if(_buffer.size() < sizeof(T)){
            _buffer = temporary_buffer<char>(sizeof(T));
        }
        std::copy_n(reinterpret_cast<char*>(&obj), sizeof(T), _buffer.get_write());
        _data_len = sizeof(T);
    }

    size_t data_len(){
        return _data_len;
    }
    const char* data(){
        assert(_data_len!=0);
        return _buffer.get();
    }

    // OK, this API is very useful, but extremely dangerous to call.
    // Make sure that you know what is saved in this buffer!!
    template<typename T>
    const T& data(){
        static_assert(std::is_pod<T>::value, "The provided object is not a plain-old-datatype.\n");
        assert(sizeof(T) == _data_len);

        auto obj_ptr = reinterpret_cast<T*>(_buffer.get_write());
        return *obj_ptr;
    }

    // This seems only to be used by test.
    size_t buf_len(){
        return _buffer.size();
    }

    net::fragment fragment(){
        return net::fragment {_buffer.get_write(), _data_len};
    }
};

#endif
