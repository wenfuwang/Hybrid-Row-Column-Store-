For anyone who just started with RocksDB (and maybe Linux also) and is trying to build the RocksDB for the first time, hope this guide can help you avoid some hassles.

My kernel is **Linux Mint 19 Tara (32 bit)**, but this guide should also work for  recent Ubuntu distributions also. Before the build, my Linux Mint has pretty much nothing other than the default software, and the following steps enabled me to run RocksDB and create DB objects, let's get started:



1. `$ sudo apt-get install build-essential`

   This enables you to have all the essential libraries for Linux to create C/C++ programs

2. `$ sudo apt-get install g++`

   Unlike gcc, the g++ compiler does not come in standard, so you might need to install g++ as well

3. `$ gcc -v`

   According to Facebook, your gcc version should be at least 4.8 to have C++ support, you might want to check just to be certain.

4. install gflags, `$ sudo apt-get install libgflags-dev`, if not work, then Facebook recommends try this link(<http://askubuntu.com/questions/312173/installing-gflags-12-04>)

5. Install snappy: `$ sudo apt-get install libsnappy-dev`.

6. Install zlib: `$ sudo apt-get install zlib1g-dev`.

7. Install bzip2: `$ sudo apt-get install libbz2-dev`.

8. Install lz4: `$ sudo apt-get install liblz4-dev`.

9. Install zstandard: `$ sudo apt-get install libzstd-dev`.

10. (Alternative to step 4~9) `$ sudo apt-get install libgflags-dev  libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev`

11. Install git(optional): `$ sudo apt-get install git`

12. Clone RocksDB repository `$ git clone https://github.com/facebook/rocksdb.git`

13. move to the RocksDB folder `$ cd rocksdb`

14. install RocksDB `$ sudo make shared_lib`, apparently there are several compiling options when it comes to build RocksDB, reader come get more info at (https://github.com/facebook/rocksdb/blob/master/INSTALL.md), for the purpose of this guide, the `shared_lib` is good

    ...

    The build process can take up to 30 minutes depending on your system, have a coffee and relax a bit. 

    ...

15. `export LD_LIBRARY_PATH=/usr/local/lib` 

16. Create a sample program `mytest.cpp` as follows, save it within the folder

```c++
/*file name is mytest.cpp, path is /home/username/rocksdb*/
#include <assert.h> 
#include "rocksdb/db.h"
#include <iostream>
using namespace std;
int main()
{
	rocksdb::DB* db;
	rocksdb::Options options;
	options.create_if_missing = true;
	rocksdb::Status status =
  	rocksdb::DB::Open(options, "/tmp/testdb", &db);
	assert(status.ok()); 
	cout<<"First rocksdb object successfully created"<<endl;
	delete db;
	return 0;
}

```

16. Within the rocksdb folder, compile the mytest.cpp using g++

     `$ g++ -c -I./include -std=c++11 mytest.cpp`

17. Specify the library you need to link 

    `$g++ -o mytest mytest.o -L. -lrocksdb -lsnappy -lpthread -lbz2 -lz -lrt`

18. run the compiled output `$ ./mytest`, you should expect to see the output `First rocksdb object successfully created`

19. Be sure to also check the example usage in sub folder `example` (path is `/home/username/rocksdb/example`) : `$make all`