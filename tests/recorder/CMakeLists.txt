# =============================================================================
# modules/recorder：WAV 头生成的 host 单测（不碰硬件）
# =============================================================================

add_executable(test_wav_header
    test_wav.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/recorder/wav.c
)

target_include_directories(test_wav_header PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/recorder
)

add_test(NAME wav_header COMMAND test_wav_header)
