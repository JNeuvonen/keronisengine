// concurrent_queue_include_helper.h
#pragma once

#if __has_include(<concurrentqueue/concurrentqueue.h>)
#   include <concurrentqueue/concurrentqueue.h>
#elif __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#   include <concurrentqueue/moodycamel/concurrentqueue.h>
#else
#   error "ConcurrentQueue header not found"
#endif

#if __has_include(<concurrentqueue/blockingconcurrentqueue.h>)
#   include <concurrentqueue/blockingconcurrentqueue.h>
#elif __has_include(<concurrentqueue/moodycamel/blockingconcurrentqueue.h>)
#   include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#else
#   error "BlockingConcurrentQueue header not found"
#endif
