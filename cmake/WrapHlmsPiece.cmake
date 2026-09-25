# WrapHlmsPiece.cmake — turn plain GLSL files into ONE Hlms library piece
# (PHOTON-READER-1: the one voxel radiance reader).
#
#   cmake -DPIECE=<name> -DOUT=<file.any> -DIN0=<a.glsl> [-DIN1=<b.glsl> ... -DIN7=] -P WrapHlmsPiece.cmake
#   cmake -DRAW=1 -DOUT=<file.glsl> -DIN0=... -P WrapHlmsPiece.cmake
#   cmake -DUNWRAP=1 -DOUT=<file.glsl> -DIN0=<a_piece_all.any> [-DIN1=...] -P WrapHlmsPiece.cmake
#
# The output is the piece header naming <name>, every input verbatim in the
# order given, the piece end - or, with RAW, the inputs alone, concatenated
# (a plain GLSL program built from the same source, e.g. the parity suite's
# fragment half). The inputs are the ONE source (they are also
# compiled offline by glslang through #include), so nothing is ever hand-copied
# into Hlms media.
#
# UNWRAP is the other direction (PHOTON-CARDS-5): a piece of the FORK's media
# that is plain GLSL between its piece header and its piece end (JahBrdf,
# JahDiffuseAlbedo) is handed to glslang as that GLSL — the header line and the
# end line dropped, and ANY other directive mark left inside refused, so the ray
# jobs read the fork's own text and never a copy of it.
#
# THE AT-SIGN RULE IS ENFORCED HERE, not remembered: the Hlms parser executes
# its directive mark wherever it finds it, comments included (a stray one has
# rendered whole scenes black with nothing in any log — DOCS/traps/ENGINE.md),
# so an input that contains the character anywhere fails the BUILD.
if((NOT PIECE AND NOT RAW AND NOT UNWRAP) OR NOT OUT OR NOT IN0)
    message(FATAL_ERROR "WrapHlmsPiece: PIECE (or RAW or UNWRAP), OUT and IN0 are required")
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
if(UNWRAP)
    foreach(_in IN LISTS _inputs)
        file(READ "${_in}" _text)
        string(REPLACE "\r" "" _text "${_text}")
        string(REGEX REPLACE "^[ \t]*@piece\\([^)]*\\)[^\n]*\n" "" _text "${_text}")
        string(REGEX REPLACE "\n[ \t]*@end[ \t]*\n*$" "\n" _text "${_text}")
        string(FIND "${_text}" "@" _at)
        if(NOT _at EQUAL -1)
            message(FATAL_ERROR "WrapHlmsPiece: ${_in} holds a directive besides its piece "
                                "header and end (offset ${_at}); it cannot be read as plain GLSL")
        endif()
        string(APPEND _body "// unwrapped from ${_in}\n${_text}\n")
    endforeach()
    # An include guard named after the output, so two includers cannot define
    # the functions twice.
    get_filename_component(_guard "${OUT}" NAME_WE)
    string(TOUPPER "JAH_UNWRAPPED_${_guard}" _guard)
    file(WRITE "${OUT}" "#ifndef ${_guard}\n#define ${_guard}\n${_body}#endif\n")
    return()
endif()
foreach(_in IN LISTS _inputs)
    file(READ "${_in}" _text)
    string(FIND "${_text}" "@" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "WrapHlmsPiece: ${_in} contains the Hlms directive mark "
                            "at offset ${_at}; it becomes Hlms input — spell it out in words")
    endif()
    string(APPEND _body "${_text}\n")
endforeach()
if(RAW)
    file(WRITE "${OUT}" "${_body}")
else()
    file(WRITE "${OUT}" "@piece( ${PIECE} )\n${_body}@end\n")
endif()
