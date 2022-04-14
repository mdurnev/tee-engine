Use AFL for fuzzing a TA
========================

American fuzzy lop (AFL) is a free software fuzzer that employs genetic algorithms in order to efficiently increase code coverage of the test cases.
The AFL source code is located at https://github.com/google/AFL

1. Install AFL before building Open-TEE.
2. Add a pair of CA and TA for testing (or use existing).
3. Configure a new build with the autogen.sh script.
4. Build Open-TEE and install it to /opt/OpenTee
5. Start fuzzing with the fuzz script as
	./fuzz <name of your CA from /opt/OpenTee/bin>

$ repo init -u https://github.com/Open-TEE/manifest.git
$ repo sync -j10
$ mkdir build
$ cd build
$ ../autogen.sh
$ make -j
$ sudo make install
$ cd ../emulator/fuzz
$ ./fuzz test_app
