# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2025   NXP Semiconductors

#!/bin/bash
set -x

# Script apply clang-formating to all .c and .h files
# uing .clang-format derived from default clang-format version 6.0.0-1ubuntu2 (tags/RELEASE_600/final)
# main modification 
# AlignEscapedNewlines: Left
# ColumnLimit:     132
# IndentWidth:     4

# List of file extensions to process (modify as needed)
EXTENSIONS=("c" "h")

# Find and process files
for ext in "${EXTENSIONS[@]}"; do
    find ./$1 -type f -name "*.${ext}"  -exec clang-format -i {} \;
done
