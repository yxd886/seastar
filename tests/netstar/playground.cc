#include "netstar/mica/util/hash.h"

int main(){
    int num = 512;
    auto hash = mica::util::hash_cityhash(&num, sizeof(num));
    return hash;
}
