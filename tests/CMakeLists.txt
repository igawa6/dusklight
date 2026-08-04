# Host-side unit tests.
#
# Only for code that can be exercised without the game: no ISO, no renderer, no
# device. The guide converter qualifies — it is a pure string-to-document
# transform — which is why it is first.
#
# gtest comes from aurora, which already fetches it for its own tests, so this
# adds no new dependency.

include(GoogleTest)

if (NOT TARGET gtest)
  message(STATUS "dusklight: gtest unavailable, skipping tests")
  return()
endif ()

# Compiled directly rather than linked against the game: convert.cpp depends on
# nothing but guide_doc.hpp and the standard library, and linking the real
# target would drag in the whole engine for a string transform.
add_executable(dusk_guide_tests
  guide_convert_test.cpp
  ../src/dusk/guide/convert.cpp
)
target_include_directories(dusk_guide_tests PRIVATE ../src ../include)
target_link_libraries(dusk_guide_tests PRIVATE gtest gtest_main)

# Optional check against a page actually saved from the in-app browser. Real
# pages are not committed — they are third-party content — so point this at one
# locally if you want the coverage:
#   cmake -DGUIDE_TEST_PAGE=/path/to/saved.html ...
if (DEFINED GUIDE_TEST_PAGE AND EXISTS "${GUIDE_TEST_PAGE}")
  target_compile_definitions(dusk_guide_tests PRIVATE GUIDE_TEST_PAGE="${GUIDE_TEST_PAGE}")
endif ()

gtest_discover_tests(dusk_guide_tests)
