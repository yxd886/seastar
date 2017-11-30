/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"

#include "netstar/extendable_buffer.hh"

using namespace seastar;
using namespace netstar;

struct fst_obj{
    char x[5];
};

struct snd_obj{
    char x[5];
    int i;
    int j;
};

int main(int ac, char** av) {
    app_template app;

    return app.run_deprecated(ac, av, [&app] {
        fst_obj o1;
        o1.x[0] = 'f';
        o1.x[1] = 'u';
        o1.x[2] = 'c';
        o1.x[3] = 'k';
        o1.x[4] = 'y';

        // test move assignment.
        extendable_buffer b1;
        extendable_buffer b2(sizeof(fst_obj));
        assert(b2.buf_len() == roundup<8>(sizeof(fst_obj)));
        b2.fill_data(o1);
        b1 = std::move(b2);
        assert(b1.data_len() == sizeof(fst_obj));
        assert(b2.data_len() == 0);
        assert(b2.buf_len() == 0);
        auto& o2 = b1.data<fst_obj>();
        assert(o2.x[0] == 'f');
        assert(o2.x[1] == 'u');
        assert(o2.x[2] == 'c');
        assert(o2.x[3] == 'k');
        assert(o2.x[4] == 'y');

        // test move construction.
        extendable_buffer b3(sizeof(fst_obj));
        b3.fill_data(o1);
        extendable_buffer b4(std::move(b3));
        assert(b4.data_len() == sizeof(fst_obj));
        assert(b3.data_len() == 0);
        assert(b3.buf_len() == 0);
        auto& o3 = b1.data<fst_obj>();
        assert(o3.x[0] == 'f');
        assert(o3.x[1] == 'u');
        assert(o3.x[2] == 'c');
        assert(o3.x[3] == 'k');
        assert(o3.x[4] == 'y');

        // test fill in larger object
        extendable_buffer b5(sizeof(fst_obj));
        b5.fill_data(o1);
        assert(b5.data_len() == sizeof(fst_obj));
        assert(b5.buf_len() == roundup<8>(sizeof(fst_obj)));
        snd_obj o4;
        o4.x[0] = 'a';
        o4.x[1] = 'e';
        o4.x[2] = 'i';
        o4.x[3] = 'o';
        o4.x[4] = 'u';
        o4.i = 5;
        o4.j = 6;
        b5.fill_data(o4);
        assert(b5.data_len() == sizeof(snd_obj));
        assert(b5.buf_len() == roundup<8>(sizeof(snd_obj)));
        auto& o5 = b5.data<snd_obj>();
        assert(o5.i == 5);
        assert(o5.j == 6);

        // test fill in with a smaller object
        b5.fill_data(o1);
        assert(b5.buf_len() == roundup<8>(sizeof(snd_obj)));
        assert(b5.data_len() == sizeof(fst_obj));
        auto& o6 = b5.data<fst_obj>();
        assert(o6.x[0] == 'f');
        assert(o6.x[1] == 'u');
        assert(o6.x[2] == 'c');
        assert(o6.x[3] == 'k');
        assert(o6.x[4] == 'y');

        return make_ready_future<>().then([]{
           printf("Test complete!\n");
           std::cout<<roundup<8>(sizeof(fst_obj))<<std::endl;
           std::cout<<roundup<8>(sizeof(snd_obj))<<std::endl;
           engine().exit(0);
        });
    });
}
