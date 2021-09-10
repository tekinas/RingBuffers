project('FunctionQueue','cpp', default_options : ['cpp_std=c++20','cpp_rtti=false','warning_level=3'])

threads = dependency('threads')
fmt = dependency('fmt', method : 'cmake', modules : ['fmt::fmt-header-only'])
boost = dependency('boost')
folly = dependency('folly', method : 'cmake', modules : ['Folly::folly', 'Folly::folly_deps'])
ring_buffers = declare_dependency(include_directories : 'src/include')

project_deps = declare_dependency(dependencies : [threads, fmt, folly, boost, ring_buffers]) 

executable('fq_test_1r_1w', 'src/tests/fq_test_1r_1w.cpp' , dependencies : project_deps)
executable('fq_test_mr_mw', 'src/tests/fq_test_mr_mw.cpp' , dependencies : project_deps)

executable('fq_test_call_vector', 'src/tests/fq_test_call_vector.cpp' , dependencies : project_deps)
executable('fq_test_call_queue', 'src/tests/fq_test_call_queue.cpp' , dependencies : project_deps)

executable('oq_test_1r_1w', 'src/tests/oq_test_1r_1w.cpp' , dependencies : project_deps)
executable('oq_test_nr_1w', 'src/tests/oq_test_nr_1w.cpp' , dependencies : project_deps)
