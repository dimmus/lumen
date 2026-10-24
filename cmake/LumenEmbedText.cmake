# cmake -DIN=<text file> -DOUT=<header> -P LumenEmbedText.cmake
# Writes IN as one C++ raw string literal, for `constexpr const char* k = #include "OUT";`
# (lumen_embed_glsl in LumenPlatform.cmake).
file(READ "${IN}" text)
string(FIND "${text}" ")lxglsl\"" clash)
if(NOT clash EQUAL -1)
    message(FATAL_ERROR "${IN} contains the raw string delimiter )lxglsl\"")
endif()
file(WRITE "${OUT}.tmp" "R\"lxglsl(${text})lxglsl\"\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
