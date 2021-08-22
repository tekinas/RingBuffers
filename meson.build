project('FunctionQueue','cpp', default_options : ['cpp_std=c++20','cpp_rtti=false','cpp_eh=none','warning_level=3'])

threads = dependency('threads')
boost_header_only = dependency('boost')
fmt = dependency('fmt')
folly = dependency('folly_header_only')

ring_buffers = declare_dependency(include_directories : './include')

project_deps = declare_dependency(dependencies : [threads, fmt, folly, boost_header_only, ring_buffers]) 

executable('fq_test_1r_1w', './tests/fq_test_1r_1w.cpp' , dependencies : project_deps)
executable('fq_test_mr_mw', './tests/fq_test_mr_mw.cpp' , dependencies : project_deps)

executable('fq_test_call_vector', './tests/fq_test_call_vector.cpp' , dependencies : project_deps)
executable('fq_test_call_queue', './tests/fq_test_call_queue.cpp' , dependencies : project_deps)

executable('oq_test_1r_1w', './tests/oq_test_1r_1w.cpp' , dependencies : project_deps)
executable('oq_test_nr_1w', './tests/oq_test_nr_1w.cpp' , dependencies : project_deps)
