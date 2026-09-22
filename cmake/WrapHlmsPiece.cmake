# WrapHlmsPiece.cmake — turn plain GLSL files into ONE Hlms library piece
# (PHOTON-READER-1: the one voxel radiance reader).
#
#   cmake -DPIECE=<name> -DOUT=<file.any> -DIN0=<a.glsl> [-DIN1=<b.glsl> ... -DIN7=] -P WrapHlmsPiece.cmake
#
# The output is the piece header naming <name>, every input verbatim in the
# order given, the piece end. The inputs are the ONE source (they are also
# compiled offline by glslang through #include), so nothing is ever hand-copied
# into Hlms media.
#
# THE AT-SIGN RULE IS ENFORCED HERE, not remembered: the Hlms parser executes
# its directive mark wherever it finds it, comments included (a stray one has
# rendered whole scenes black with nothing in any log — DOCS/traps/ENGINE.md),
# so an input that contains the character anywhere fails the BUILD.
if(NOT PIECE OR NOT OUT OR NOT IN0)
    message(FATAL_ERROR "WrapHlmsPiece: PIECE, OUT and IN0 are required")
endif()
# Numbered, not a list: a custom command's arguments reach a shell, where a list
# separator is a pipe or a statement end.
set(_inputs "")
foreach(_i RANGE 0 7)
    if(IN${_i})
        list(APPEND _inputs "${IN${_i}}")
    endif()
endforeach()
set(_body "")
foreach(_in IN LISTS _inputs)
    file(READ "${_in}" _text)
    string(FIND "${_text}" "@" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "WrapHlmsPiece: ${_in} contains the Hlms directive mark "
                            "at offset ${_at}; it becomes Hlms input — spell it out in words")
    endif()
    string(APPEND _body "${_text}\n")
endforeach()
file(WRITE "${OUT}" "@piece( ${PIECE} )\n${_body}@end\n")
