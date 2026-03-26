# CMake generated Testfile for 
# Source directory: /Users/jesthuecarlskov/code/fb2k
# Build directory: /Users/jesthuecarlskov/code/fb2k/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[tag_lookup_service_tests]=] "/Users/jesthuecarlskov/code/fb2k/build/tag_lookup_service_tests")
set_tests_properties([=[tag_lookup_service_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/jesthuecarlskov/code/fb2k/CMakeLists.txt;175;add_test;/Users/jesthuecarlskov/code/fb2k/CMakeLists.txt;0;")
add_test([=[album_art_service_tests]=] "/Users/jesthuecarlskov/code/fb2k/build/album_art_service_tests")
set_tests_properties([=[album_art_service_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/jesthuecarlskov/code/fb2k/CMakeLists.txt;185;add_test;/Users/jesthuecarlskov/code/fb2k/CMakeLists.txt;0;")
subdirs("_deps/nlohmann_json-build")
