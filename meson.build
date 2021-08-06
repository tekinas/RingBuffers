project('FunctionQueue','cpp', default_options : ['cpp_std=c++20','cpp_rtti=false','cpp_eh=none','warning_level=3'])

threads = dependency('threads')
tcmalloc = dependency('tcmalloc')
fmt = dependency('fmt')
boost_header_only = dependency('boost')

project_deps = declare_dependency(dependencies : [fmt, boost_header_only, tcmalloc]) 
project_deps_mt = declare_dependency(dependencies : [project_deps, threads]) 

executable('fq_test_1r_1w', 'src/tests/fq_test_1r_1w.cpp' , dependencies : project_deps_mt)
executable('fq_test_1r_nw', 'src/tests/fq_test_1r_nw.cpp' , dependencies : project_deps_mt)
executable('fq_test_nr_nw', 'src/tests/fq_test_nr_nw.cpp' , dependencies : project_deps_mt)
executable('fq_test_nr_1w', 'src/tests/fq_test_nr_1w.cpp' , dependencies : project_deps_mt)
executable('fq_test_mr_mw', 'src/tests/fq_test_mr_mw.cpp' , dependencies : project_deps_mt)
executable('fq_test_nr_nw_asio', 'src/tests/fq_test_nr_nw_asio.cpp' , dependencies : project_deps_mt)

executable('fq_test_call_vector', 'src/tests/fq_test_call_vector.cpp' , dependencies : project_deps)
executable('fq_test_call_queue', 'src/tests/fq_test_call_queue.cpp' , dependencies : project_deps)

executable('oq_test_1r_1w', 'src/tests/oq_test_1r_1w.cpp' , dependencies : project_deps_mt)
executable('oq_test_nr_1w', 'src/tests/oq_test_nr_1w.cpp' , dependencies : project_deps_mt)

executable('dq_test_1r_1w' ,'src/tests/dq_test_1r_1w.cpp' , dependencies : project_deps_mt)
