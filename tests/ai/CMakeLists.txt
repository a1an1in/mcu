# =============================================================================
# modules/ai：HTTP POST 请求构造的 host 单测（不碰网络）
# =============================================================================

add_executable(test_http_post
    test_http_post.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ai/http_post.c
)

target_include_directories(test_http_post PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ai
)

add_test(NAME http_post COMMAND test_http_post)
