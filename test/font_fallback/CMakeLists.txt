add_executable(FontFallbackTest
  FontFallbackTest.cpp
  ${REPO_ROOT}/lib/EpdFont/FontFallback.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(FontFallbackTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi
  ${CMAKE_CURRENT_SOURCE_DIR}/../minibidi_arabic/stubs
)
target_link_libraries(FontFallbackTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(FontFallbackTest)
