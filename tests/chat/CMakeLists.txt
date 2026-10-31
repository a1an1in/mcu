# =============================================================================
# modules/chat：HTTP POST 请求构造的 host 单测（不碰网络）
# =============================================================================

add_executable(test_http_post
    test_http_post.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/chat/http_post.c
)

target_include_directories(test_http_post PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/chat
)

add_test(NAME http_post COMMAND test_http_post)
