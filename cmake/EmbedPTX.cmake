# EmbedPTX.cmake
# Reads a PTX file and generates a C header with the PTX as a string constant.
# Usage: cmake -DPTX_FILE=input.ptx -DHEADER_FILE=output.h -P EmbedPTX.cmake

file(READ "${PTX_FILE}" PTX_CONTENTS)

# Escape backslashes, then quotes, then add line continuation
string(REPLACE "\\" "\\\\" PTX_CONTENTS "${PTX_CONTENTS}")
string(REPLACE "\"" "\\\"" PTX_CONTENTS "${PTX_CONTENTS}")
string(REPLACE "\n" "\\n\"\n\"" PTX_CONTENTS "${PTX_CONTENTS}")

file(WRITE "${HEADER_FILE}"
    "// Auto-generated from ${PTX_FILE} — do not edit\n"
    "#pragma once\n"
    "static const char* const kSpectralGPUKernelPTX =\n"
    "\"${PTX_CONTENTS}\";\n"
)
