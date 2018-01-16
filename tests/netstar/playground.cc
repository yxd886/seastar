#include "netstar/mica/util/hash.h"
#include <rte_ethdev.h>
#include <experimental/optional>
#include <stdio.h>

int main(){
    int num = 512;
    auto hash = mica::util::hash_cityhash(&num, sizeof(num));
    std::experimental::optional<rte_mbuf*> tester;
    printf("The size of std::experimental::optional<rte_mbuf*> is %zu\n", sizeof(tester));
    return hash;
}
